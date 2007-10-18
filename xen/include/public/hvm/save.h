/* 
 * hvm/save.h
 *
 * Structure definitions for HVM state that is held by Xen and must
 * be saved along with the domain's memory and device-model state.
 * 
 * Copyright (c) 2007 XenSource Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef __XEN_PUBLIC_HVM_SAVE_H__
#define __XEN_PUBLIC_HVM_SAVE_H__

/*
 * Structures in this header *must* have the same layout in 32bit 
 * and 64bit environments: this means that all fields must be explicitly 
 * sized types and aligned to their sizes, and the structs must be 
 * a multiple of eight bytes long.
 *
 * Only the state necessary for saving and restoring (i.e. fields 
 * that are analogous to actual hardware state) should go in this file. 
 * Internal mechanisms should be kept in Xen-private headers.
 */

/* 
 * Each entry is preceded by a descriptor giving its type and length
 */
struct hvm_save_descriptor {
    uint16_t typecode;          /* Used to demux the various types below */
    uint16_t instance;          /* Further demux within a type */
    uint32_t length;            /* In bytes, *not* including this descriptor */
};


/* 
 * Each entry has a datatype associated with it: for example, the CPU state 
 * is saved as a HVM_SAVE_TYPE(CPU), which has HVM_SAVE_LENGTH(CPU), 
 * and is identified by a descriptor with typecode HVM_SAVE_CODE(CPU).
 * DECLARE_HVM_SAVE_TYPE binds these things together with some type-system
 * ugliness.
 */

#define DECLARE_HVM_SAVE_TYPE(_x, _code, _type)                   \
  struct __HVM_SAVE_TYPE_##_x { _type t; char c[_code]; }

#define HVM_SAVE_TYPE(_x) typeof (((struct __HVM_SAVE_TYPE_##_x *)(0))->t)
#define HVM_SAVE_LENGTH(_x) (sizeof (HVM_SAVE_TYPE(_x)))
#define HVM_SAVE_CODE(_x) (sizeof (((struct __HVM_SAVE_TYPE_##_x *)(0))->c))


/* 
 * The series of save records is teminated by a zero-type, zero-length 
 * descriptor.
 */

struct hvm_save_end {};
DECLARE_HVM_SAVE_TYPE(END, 0, struct hvm_save_end);

#if defined(__i386__) || defined(__x86_64__)
#include "../arch-x86/hvm/save.h"
#else
#error "unsupported architecture"
#endif

#endif /* __XEN_PUBLIC_HVM_SAVE_H__ */
