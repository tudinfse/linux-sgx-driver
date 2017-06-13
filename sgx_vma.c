/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * GPL LICENSE SUMMARY
 *
 * Copyright(c) 2016 Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * Contact Information:
 * Jarkko Sakkinen <jarkko.sakkinen@linux.intel.com>
 * Intel Finland Oy - BIC 0357606-4 - Westendinkatu 7, 02160 Espoo
 *
 * BSD LICENSE
 *
 * Copyright(c) 2016 Intel Corporation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Intel Corporation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors:
 *
 * Jarkko Sakkinen <jarkko.sakkinen@linux.intel.com>
 * Suresh Siddha <suresh.b.siddha@intel.com>
 * Serge Ayoun <serge.ayoun@intel.com>
 * Shay Katz-zamir <shay.katz-zamir@intel.com>
 * Sean Christopherson <sean.j.christopherson@intel.com>
 */

#include "sgx.h"
#include <asm/mman.h>
#include <linux/delay.h>
#include <linux/file.h>
#include <linux/highmem.h>
#include <linux/ratelimit.h>
#include <linux/slab.h>
#include <linux/hashtable.h>
#include <linux/shmem_fs.h>
#include <linux/mm.h>

/* callback for open enclave-VMA operation, called at process fork */
static void sgx_vma_open(struct vm_area_struct *vma)
{
	/* if host app mmaped enclave memory but didn't yet ECREATE enclave, nop */
	struct sgx_encl *encl = vma->vm_private_data;
	if (!encl)
		return;

	/* kref cannot underflow because ECREATE ioctl checks that there is only
	 * one single VMA for the enclave before proceeding.
	 */
	kref_get(&encl->refcount);
}

/* callback for close enclave-VMA operation, called at process exit */
static void sgx_vma_close(struct vm_area_struct *vma)
{
	/* if host app mmaped enclave memory but didn't yet ECREATE enclave, nop */
	struct sgx_encl *encl = vma->vm_private_data;
	if (!encl)
		return;

	/* remove PTEs for all pages of enclave in VMA and mark enclave dead */
	mutex_lock(&encl->lock);
	zap_vma_ptes(vma, vma->vm_start, vma->vm_end - vma->vm_start);
	encl->flags |= SGX_ENCL_DEAD;
	mutex_unlock(&encl->lock);
	/* actually release enclave if not used by any process */
	kref_put(&encl->refcount, sgx_encl_release);
}

/* callback for fault enclave-VMA operation, at each in-enclave page fault */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0))
static int sgx_vma_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
#else
static int sgx_vma_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
#endif
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4,10,0))
	unsigned long addr = (unsigned long)vmf->address;
#else
	unsigned long addr = (unsigned long) vmf->virtual_address;
#endif
	struct sgx_encl_page *entry;

	/* handle page fault via ELDU, do not retry if no free EPC pages */
	entry = sgx_fault_page(vma, addr, 0);

	/* NOTE: even if there is no free EPC page (EBUSY), still no error */
	if (!IS_ERR(entry) || PTR_ERR(entry) == -EBUSY)
		return VM_FAULT_NOPAGE;
	else
		return VM_FAULT_SIGBUS;
}

/**
 * sgx_vma_access_word - debug read/write 64/32-bit word via EDBGRD/EDBGWR
 *
 * @encl:		corresponding enclave
 * @addr:		virtual address to read/write
 * @buf:		where to put EDBGRD result or from where to read EDBGWR input
 * @len:		length of buffer
 * @write:		true - execute EDBGWR, false - execute EDBGRD
 * @encl_page:	enclave page where to read/write
 * @i:			offset of buffer
 *
 * Reads/writes into/from buf in debug mode by executing ENCLS(EDBGRD/EDBGWR).
 * Takes into account accesses not perfectly aligned to 64/32-bit word.
 */
static inline int sgx_vma_access_word(struct sgx_encl *encl,
				      unsigned long addr,
				      void *buf,
				      int len,
				      int write,
				      struct sgx_encl_page *encl_page,
				      int i)
{
	char data[sizeof(unsigned long)];
	int align, cnt, offset;
	void *vaddr;
	int ret;

	/* find out how many bytes need to be read/written */
	offset = ((addr + i) & (PAGE_SIZE - 1)) & ~(sizeof(unsigned long) - 1);
	align = (addr + i) & (sizeof(unsigned long) - 1);
	cnt = sizeof(unsigned long) - align;
	cnt = min(cnt, len - i);

	if (write) {
		/* disallow writing to any field of TCS page except flags */
		if (encl_page->flags & SGX_ENCL_PAGE_TCS &&
		    (offset < 8 || (offset + (len - i)) > 16))
			return -ECANCELED;

		/* if write is not perfectly aligned or not exactly 64/32-bit */
		if (align || (cnt != sizeof(unsigned long))) {
			/* then need to augment write with data residing in other bits */
			vaddr = sgx_get_page(encl_page->epc_page);
			ret = __edbgrd((void *)((unsigned long)vaddr + offset),
				       (unsigned long *)data);
			sgx_put_page(vaddr);
			if (ret) {
				sgx_dbg(encl, "EDBGRD returned %d\n", ret);
				return -EFAULT;
			}
		}

		/* copy EDBGWR input to data and execute EDBGWR instruction */
		memcpy(data + align, buf + i, cnt);
		vaddr = sgx_get_page(encl_page->epc_page);
		ret = __edbgwr((void *)((unsigned long)vaddr + offset),
			       (unsigned long *)data);
		sgx_put_page(vaddr);
		if (ret) {
			sgx_dbg(encl, "EDBGWR returned %d\n", ret);
			return -EFAULT;
		}
	} else {
		/* disallow reading reserved fields of TCS page */
		if (encl_page->flags & SGX_ENCL_PAGE_TCS &&
		    (offset + (len - i)) > 72)
			return -ECANCELED;

		/* execute EDBGRD instruction and copy result in buf */
		vaddr = sgx_get_page(encl_page->epc_page);
		ret = __edbgrd((void *)((unsigned long)vaddr + offset),
			       (unsigned long *)data);
		sgx_put_page(vaddr);
		if (ret) {
			sgx_dbg(encl, "EDBGRD returned %d\n", ret);
			return -EFAULT;
		}

		memcpy(buf + i, data + align, cnt);
	}

	return cnt;
}

/* callback for access enclave-VMA operation, for ptrace (thus gdb) peeks */
static int sgx_vma_access(struct vm_area_struct *vma, unsigned long addr,
			  void *buf, int len, int write)
{
	struct sgx_encl *encl = vma->vm_private_data;
	struct sgx_encl_page *entry = NULL;
	const char *op_str = write ? "EDBGWR" : "EDBGRD";
	int ret = 0;
	int i;

	/* If process was forked, VMA is still there but vm_private_data is set
	 * to NULL.
	 */
	if (!encl)
		return -EFAULT;

	if (!(encl->flags & SGX_ENCL_DEBUG) ||
	    !(encl->flags & SGX_ENCL_INITIALIZED) ||
	    (encl->flags & SGX_ENCL_DEAD))
		return -EFAULT;

	sgx_dbg(encl, "%s addr=0x%lx, len=%d\n", op_str, addr, len);

	/* since EDBGRD/EDBGWR access only 64/32-bit words, issue them in loop */
	for (i = 0; i < len; i += ret) {
		/* just in case, perform page fault to get enclave page into EPC */
		if (!entry || !((addr + i) & (PAGE_SIZE - 1))) {
			if (entry)
				entry->flags &= ~SGX_ENCL_PAGE_RESERVED;

			entry = sgx_fault_page(vma, (addr + i) & PAGE_MASK,
					       SGX_FAULT_RESERVE);
			if (IS_ERR(entry)) {
				ret = PTR_ERR(entry);
				entry = NULL;
				break;
			}
		}

		/* No locks are needed because used fields are immutable after
		 * intialization.
		 */
		ret = sgx_vma_access_word(encl, addr, buf, len, write,
					  entry, i);
		if (ret < 0)
			break;
	}

	if (entry)
		entry->flags &= ~SGX_ENCL_PAGE_RESERVED;

	return (ret < 0 && ret != -ECANCELED) ? ret : i;
}

/* VMA operations registered by driver on its mmap, see sgx_mmap() */
const struct vm_operations_struct sgx_vm_ops = {
	.close = sgx_vma_close,
	.open = sgx_vma_open,
	.fault = sgx_vma_fault,
	.access = sgx_vma_access,
};
