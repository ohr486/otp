/*
 * %CopyrightBegin%
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright Ericsson AB 2008-2025. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * %CopyrightEnd%
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif
#include "sys.h"
#include "erl_vm.h"
#include "global.h"
#include "erl_process.h"
#include "error.h"
#include "bif.h"
#include "erl_binary.h"
#include "big.h"

#include "erl_unicode.h"
#include "erl_unicode_normalize.h"


typedef struct _restart_context {
    byte *bytes;
    Uint num_processed_bytes;
    Uint num_bytes_to_process;
    Uint num_resulting_chars;
    int state;
} RestartContext;


#define LOOP_FACTOR 10
#define LOOP_FACTOR_SIMPLE 50 /* When just counting */

static Uint max_loop_limit;

static BIF_RETTYPE utf8_to_list(Process *p, Eterm arg1);
static BIF_RETTYPE finalize_list_to_list(Process *p, 
					 byte *bytes,
					 Eterm rest,
					 Uint num_processed_bytes,
					 Uint num_bytes_to_process, 
					 Uint num_resulting_chars, 
					 int state, Sint left,
					 Eterm tail);
static BIF_RETTYPE characters_to_utf8_trap(BIF_ALIST_3);
static BIF_RETTYPE characters_to_list_trap_1(BIF_ALIST_3);
static BIF_RETTYPE characters_to_list_trap_2(BIF_ALIST_3);

static BIF_RETTYPE characters_to_list_trap_3(BIF_ALIST_3);
static BIF_RETTYPE characters_to_list_trap_4(BIF_ALIST_1);

static Export characters_to_utf8_trap_exp;
static Export characters_to_list_trap_1_exp;
static Export characters_to_list_trap_2_exp;

static Export characters_to_list_trap_3_exp;
static Export characters_to_list_trap_4_exp;

static Export *c_to_b_int_trap_exportp = NULL;
static Export *c_to_l_int_trap_exportp = NULL;

void erts_init_unicode(void)
{
    max_loop_limit = CONTEXT_REDS * LOOP_FACTOR;
    /* Non visual BIFs to trap to. */
    erts_init_trap_export(&characters_to_utf8_trap_exp,
			  am_erlang, ERTS_MAKE_AM("characters_to_utf8_trap"), 4,
			  &characters_to_utf8_trap);

    erts_init_trap_export(&characters_to_list_trap_1_exp,
			  am_erlang, ERTS_MAKE_AM("characters_to_list_trap_1"), 3,
			  &characters_to_list_trap_1);

    erts_init_trap_export(&characters_to_list_trap_2_exp,
			  am_erlang, ERTS_MAKE_AM("characters_to_list_trap_2"), 3,
			  &characters_to_list_trap_2);

    erts_init_trap_export(&characters_to_list_trap_3_exp,
			  am_erlang, ERTS_MAKE_AM("characters_to_list_trap_3"), 3,
			  &characters_to_list_trap_3);

    erts_init_trap_export(&characters_to_list_trap_4_exp,
			  am_erlang, ERTS_MAKE_AM("characters_to_list_trap_4"), 1,
			  &characters_to_list_trap_4);

    c_to_b_int_trap_exportp =  erts_export_put(am_unicode,am_characters_to_binary_int,2);
    c_to_l_int_trap_exportp =  erts_export_put(am_unicode,am_characters_to_list_int,2);
    

}


static ERTS_INLINE void *alloc_restart(size_t size)
{
    return erts_alloc(ERTS_ALC_T_UNICODE_BUFFER,size);
}

static ERTS_INLINE void free_restart(void *ptr)
{
    erts_free(ERTS_ALC_T_UNICODE_BUFFER, ptr);
}

static void cleanup_restart_context(RestartContext *rc)
{
    if (rc->bytes != NULL) {
	free_restart(rc->bytes);
	rc->bytes = NULL;
    }
}

static int cleanup_restart_context_bin(Binary *bp)
{
    RestartContext *rc = ERTS_MAGIC_BIN_DATA(bp);
    cleanup_restart_context(rc);
    return 1;
}

static RestartContext *get_rc_from_bin(Eterm mref)
{
    Binary *mbp = erts_magic_ref2bin(mref);
    ASSERT(ERTS_MAGIC_BIN_DESTRUCTOR(mbp)
	   == cleanup_restart_context_bin);
    return (RestartContext *) ERTS_MAGIC_BIN_DATA(mbp);    
}

static Eterm make_magic_bin_for_restart(Process *p, RestartContext *rc)
{
    Binary *mbp = erts_create_magic_binary(sizeof(RestartContext),
					   cleanup_restart_context_bin);
    RestartContext *restartp = ERTS_MAGIC_BIN_DATA(mbp);
    Eterm *hp;
    sys_memcpy(restartp,rc,sizeof(RestartContext));
    hp = HAlloc(p, ERTS_MAGIC_REF_THING_SIZE);
    return erts_mk_magic_ref(&hp, &MSO(p), mbp);
}

	
Sint erts_unicode_set_loop_limit(Sint limit) 
{
    Sint save = (Sint) max_loop_limit;
    if (limit <= 0) {
	max_loop_limit = CONTEXT_REDS * LOOP_FACTOR;
    } else {
	max_loop_limit = (Uint) limit;
    }
    return save;
}

static ERTS_INLINE int allowed_iterations(Process *p)
{
    int tmp = ERTS_BIF_REDS_LEFT(p) * LOOP_FACTOR;
    int tmp2 = max_loop_limit;
    if (tmp2 < tmp)
	return tmp2;
    else
	return tmp;
}

static ERTS_INLINE void cost_to_proc(Process *p, Sint cost)
{
    Sint x = (cost / LOOP_FACTOR);
    BUMP_REDS(p,x);
}

static ERTS_INLINE int simple_loops_to_common(int cost)
{
    int factor = (LOOP_FACTOR_SIMPLE / LOOP_FACTOR);
    return (cost / factor);
}

static Sint aligned_binary_size(Eterm binary)
{
    Uint size = bitstring_size(binary);

    if (TAIL_BITS(size) == 0 && size <= ERTS_SINT_MAX) {
        return (Sint)size;
    }

    return -1;
}

static Sint latin1_binary_need(Eterm binary)
{
    const byte *temp_alloc = NULL, *bytes;
    Uint size;
    Sint need;
    Sint i;

    bytes = erts_get_aligned_binary_bytes(binary, &size, &temp_alloc);
    if (bytes == NULL) {
        return -1;
    }

    for(i = 0, need = 0; i < size; ++i) {
        if (bytes[i] & ((byte) 0x80)) {
            need += 2;
        } else {
            need += 1;
        }
    }

    erts_free_aligned_binary_bytes(temp_alloc);
    return need;
}

static int utf8_len(byte first) 
{
    if ((first & ((byte) 0x80)) == 0) {
	return 1;
    } else if ((first & ((byte) 0xE0)) == 0xC0) {
	return 2;
    } else if ((first & ((byte) 0xF0)) == 0xE0) {
	return 3;
    } else if ((first & ((byte) 0xF8)) == 0xF0) {
	return 4;
    } 
    return -1;
}

static Uint copy_utf8_bin(byte *target, const byte *source, Uint size,
			  byte *leftover, int *num_leftovers,
			  const byte **err_pos, Uint *characters)
{
    Uint copied = 0;
    if (leftover != NULL && *num_leftovers) {
	int need = utf8_len(leftover[0]);
	int from_source = need - (*num_leftovers);
	Uint c;
	const byte *tmp_err_pos = NULL;
	ASSERT(need > 0);
	ASSERT(from_source > 0);
	if (size < from_source) {
	    sys_memcpy(leftover + (*num_leftovers), source, size);
	    *num_leftovers += size;
	    return 0;
	}
	/* leftover has room for four bytes (see bif) */
	sys_memcpy(leftover + (*num_leftovers),source,from_source);
	c = copy_utf8_bin(target, leftover, need, NULL, NULL, &tmp_err_pos, characters);
	if (tmp_err_pos != 0) {
	    *err_pos = source;
	    return 0;
	}
	copied += c;
	*num_leftovers = 0;
	size -= from_source;
	target += c;
	source += from_source;
    }
    while (size) {
	if (((*source) & ((byte) 0x80)) == 0) {
	    *(target++) = *(source++);
	    --size; ++copied;
	} else if (((*source) & ((byte) 0xE0)) == 0xC0) {
	    if (leftover && size < 2) {
		*leftover = *source;
		*num_leftovers = 1;
		break;
	    }
	    if (size < 2 || ((source[1] & ((byte) 0xC0)) != 0x80) ||
		((*source) < 0xC2) /* overlong */) {
		*err_pos = source;
		return copied;
	    }
	    *(target++) = *(source++);
	    *(target++) = *(source++);
	    size -= 2; copied += 2;
	} else if (((*source) & ((byte) 0xF0)) == 0xE0) {
	    if (leftover && size < 3) {
		sys_memcpy(leftover, source, (int) size);
		*num_leftovers = (int) size;
		break;
	    }
	    if (size < 3 || ((source[1] & ((byte) 0xC0)) != 0x80) ||
		((source[2] & ((byte) 0xC0)) != 0x80) ||
		(((*source) == 0xE0) && (source[1] < 0xA0)) /* overlong */ ) {
		*err_pos = source;
		return copied;
	    }
	    if ((((*source) & ((byte) 0xF)) == 0xD) && 
		((source[1] & 0x20) != 0)) {
		*err_pos = source;
		return copied;
	    }

	    *(target++) = *(source++);
	    *(target++) = *(source++);
	    *(target++) = *(source++);
	    size -= 3; copied += 3;
	} else if (((*source) & ((byte) 0xF8)) == 0xF0) {
	    if (leftover && size < 4) {
		sys_memcpy(leftover, source, (int) size);
		*num_leftovers = (int) size;
		break;
	    }
	    if (size < 4 || ((source[1] & ((byte) 0xC0)) != 0x80) ||
		((source[2] & ((byte) 0xC0)) != 0x80) ||
		((source[3] & ((byte) 0xC0)) != 0x80) ||
		(((*source) == 0xF0) && (source[1] < 0x90)) /* overlong */) {
		*err_pos = source;
		return copied;
	    }
	    if ((((*source) & ((byte)0x7)) > 0x4U) ||
		((((*source) & ((byte)0x7)) == 0x4U) && 
		 ((source[1] & ((byte)0x3F)) > 0xFU))) {
		*err_pos = source;
		return copied;
	    }
	    *(target++) = *(source++);
	    *(target++) = *(source++);
	    *(target++) = *(source++);
	    *(target++) = *(source++);
	    size -= 4; copied +=4;
	} else {
	    *err_pos = source;
	    return copied;
	}
	++(*characters);
    }
    return copied;
}
	    
	    
    
static Sint utf8_need(Eterm ioterm, int latin1, Uint *costp) 
{
    Eterm *objp;
    Eterm obj;
    DECLARE_ESTACK(stack);
    Sint need = 0;
    Uint cost = 0;

    if (is_nil(ioterm)) {
	DESTROY_ESTACK(stack);
	*costp = 0;
	return need;
    }
    if(is_bitstring(ioterm)) {
	DESTROY_ESTACK(stack);
	if (latin1) {
	    Sint x = latin1_binary_need(ioterm);
	    *costp = x;
	    return x;
	} else {
	    *costp = 1;
	    return aligned_binary_size(ioterm);
	}
    }
    
    if (!is_list(ioterm)) {
	DESTROY_ESTACK(stack);
	*costp = 0;
	return (Sint) -1;
    }
    /* OK a list, needs to be processed in order, handling each flat list-level
       as they occur, just like io_list_to_binary would */
    ESTACK_PUSH(stack,ioterm);
    while (!ESTACK_ISEMPTY(stack)) {
	ioterm = ESTACK_POP(stack);
	if (is_nil(ioterm)) {
	    /* ignore empty lists */
	    continue;
	}
	if(is_list(ioterm)) {
L_Again:   /* Restart with sublist, old listend was pushed on stack */
	    objp = list_val(ioterm);
	    obj = CAR(objp);
	    for(;;) { /* loop over one flat list of bytes and binaries
		         until sublist or list end is encountered */
		if (is_small(obj)) { /* Always small */
		    for(;;) {
			Uint x = unsigned_val(obj);
			if (x < 0x80)
			    need +=1;
			else if (x < 0x800)
			    need += 2;
			else if (x < 0x10000) 
			    need += 3;
			else 
			    need += 4; 
			/* everything else will give badarg later 
			   in the process, so we dont check */
			++cost;
			ioterm = CDR(objp);
			if (!is_list(ioterm)) {
			    break;
			}
			objp = list_val(ioterm);
			obj = CAR(objp);
			if (!is_small(obj))
			    break;
		    }
		} else if (is_nil(obj)) {
		    ioterm = CDR(objp);
		    if (!is_list(ioterm)) {
			break;
		    }
		    objp = list_val(ioterm);
		    obj = CAR(objp);
		} else if (is_list(obj)) {
		    /* push rest of list for later processing, start 
		       again with sublist */
		    ESTACK_PUSH(stack,CDR(objp));
		    ioterm = obj;
		    goto L_Again;
		} else if (is_bitstring(obj)) {
		    Sint x;

		    if (latin1) { 
			x = latin1_binary_need(obj);
			if (x < 0) {
			    DESTROY_ESTACK(stack);
			    *costp = cost;
			    return x;
			} 
			cost += x;
		    } else {
			x = aligned_binary_size(obj);
			if (x < 0) {
			    DESTROY_ESTACK(stack);
			    *costp = cost;
			    return x;
			} 
			++cost;
		    }
		    need += x;
		    ioterm = CDR(objp);
		    if (is_list(ioterm)) {
			/* objp and obj need to be updated if 
			   loop is to continue */
			objp = list_val(ioterm);
			obj = CAR(objp);
		    }
                } else if (is_big(obj)) {
                    /*
                     * This is obviously an error, but we
                     * need do_build_utf8() to produce the
                     * error; otherwise, we will generate
                     * a badarg instead of the informative
                     * error tuple.
                     */
		    DESTROY_ESTACK(stack);
                    *costp = cost;
                    return need;
		} else {
		    DESTROY_ESTACK(stack);
		    *costp = cost;
		    return ((Sint) -1);
		} 
		if (is_nil(ioterm) || !is_list(ioterm)) {
		    break;
		}
	    } /* for(;;) */
	} /* is_list(ioterm) */
	
	if (!is_list(ioterm) && !is_nil(ioterm)) {
	    /* improper list end */
	    if (is_bitstring(ioterm)) {
		Sint x; 
		if (latin1) {
		    x = latin1_binary_need(ioterm);
		    if (x < 0) {
			DESTROY_ESTACK(stack);
			*costp = cost;
			return x;
		    } 
		    cost += x;
		} else {
		    x = aligned_binary_size(ioterm);
		    if (x < 0) {
			DESTROY_ESTACK(stack);
			*costp = cost;
			return x;
		    } 
		    ++cost;
		}
		need += x;
	    } else {
		DESTROY_ESTACK(stack);
		*costp = cost;
		return ((Sint) -1);
	    }
	}
    } /* while  not estack empty */
    DESTROY_ESTACK(stack);
    *costp = cost;
    return need;
}
    
    
static Eterm do_build_utf8(Process *p, Eterm ioterm, Sint *left, int latin1,
			   byte *target, Uint *pos, Uint *characters, int *err,
			   byte *leftover, int *num_leftovers)
{
    int c;
    Eterm *objp;
    Eterm obj;
    DECLARE_ESTACK(stack);

    *err = 0;
    if ((*left) <= 0 || is_nil(ioterm)) {
	DESTROY_ESTACK(stack);
	return ioterm;
    }
    if(is_bitstring(ioterm)) {
        const byte *temp_alloc = NULL, *bytes;
        Uint i, orig_size, size;
        Eterm res_term = NIL;

        bytes = erts_get_aligned_binary_bytes(ioterm, &orig_size, &temp_alloc);
        if (bytes == NULL) {
            *err = 1;
            DESTROY_ESTACK(stack);
            return ioterm;
        }

        size = orig_size;

	/* This is done to avoid splitting binaries in two 
	   and then create an unnecessary rest that eventually gives an error.
	   For cases where errors are not returned this is unnecessary */
	if (!latin1) { 
	    /* Find a valid character boundary */
	    while (size > (*left) && 
		   (((byte) bytes[(*left)]) & ((byte) 0xC0)) == ((byte) 0x80)) {
		++(*left);
	    }
	}

        if (size > (*left)) {
            /* Split the binary in two parts, of which we only process the
             * first */
            res_term = erts_make_sub_binary(p, ioterm, (*left), size - (*left));
            size = (*left);
        }

	if (!latin1) {
	    Uint num;
	    const byte *err_pos = NULL;
	    num = copy_utf8_bin(target + (*pos), bytes, 
				size, leftover, num_leftovers,&err_pos,characters);
	    *pos += num;
	    if (err_pos != NULL) {
                Uint rest_bin_offset, rest_bin_size;

                *err = 1;

                /* we have no real stack, just build a list of the binaries
                 * we have not decoded... */
                DESTROY_ESTACK(stack);

                rest_bin_offset = (err_pos - bytes);
                rest_bin_size = orig_size - rest_bin_offset;

                erts_free_aligned_binary_bytes(temp_alloc);
                return erts_make_sub_binary(p,
                                            ioterm,
                                            rest_bin_offset,
                                            rest_bin_size);
            }
	} else {
	    i = 0;
	    while(i < size) {
		if (bytes[i] < 0x80) {
		    target[(*pos)++] = bytes[i++];
		} else {
		    target[(*pos)++] = ((bytes[i] >> 6) | ((byte) 0xC0));
		    target[(*pos)++] = ((bytes[i] & 0x3F) | ((byte) 0x80));
		    ++i;
		}
		++(*characters);
	    }
	}
	*left -= size;
	DESTROY_ESTACK(stack);
	erts_free_aligned_binary_bytes(temp_alloc);
	return res_term;
    }
	
    if (!is_list(ioterm)) {
	*err = 1;
	goto done;
    }

    /* OK a list, needs to be processed in order, handling each flat list-level
       as they occur, just like io_list_to_binary would */
    ESTACK_PUSH(stack,ioterm);
    while (!ESTACK_ISEMPTY(stack) && (*left)) {
	ioterm = ESTACK_POP(stack);
	if (is_nil(ioterm)) {
	    /* ignore empty lists */
	    continue;
	}
	if(is_list(ioterm)) {
L_Again:   /* Restart with sublist, old listend was pushed on stack */
	    objp = list_val(ioterm);
	    obj = CAR(objp);
	    for(;;) { /* loop over one flat list of bytes and binaries
		         until sublist or list end is encountered */
		if (is_small(obj)) { /* Always small in unicode*/
		    if (*num_leftovers) {
			/* Have rest from previous bin and this is an integer, not allowed */
			*err = 1;
			goto done;
		    }
		    for(;;) {
			Uint x = unsigned_val(obj);
			if (latin1 && x > 255) {
			    *err = 1;
			    goto done;
			}
			if (x < 0x80) {
			    target[(*pos)++] = (byte) x;
			}
			else if (x < 0x800) {
			    target[(*pos)++] = (((byte) (x >> 6)) | 
						 ((byte) 0xC0));
			    target[(*pos)++] = (((byte) (x & 0x3F)) | 
						((byte) 0x80));
			} else if (x < 0x10000) {
			    if (x >= 0xD800 && x <= 0xDFFF) {
				/* Invalid unicode range */
				*err = 1;
				goto done;
			    }
			    target[(*pos)++] = (((byte) (x >> 12)) | 
						((byte) 0xE0));
			    target[(*pos)++] = ((((byte) (x >> 6)) & 0x3F)  | 
						((byte) 0x80));
			    target[(*pos)++] = (((byte) (x & 0x3F)) | 
						((byte) 0x80));
			} else if (x < 0x110000) { /* Standard imposed max */
			    target[(*pos)++] = (((byte) (x >> 18)) | 
						((byte) 0xF0));
			    target[(*pos)++] = ((((byte) (x >> 12)) & 0x3F)  | 
						((byte) 0x80));
			    target[(*pos)++] = ((((byte) (x >> 6)) & 0x3F)  | 
						((byte) 0x80));
			    target[(*pos)++] = (((byte) (x & 0x3F)) | 
						((byte) 0x80));
			} else {
				*err = 1;
				goto done;
			}
			++(*characters);
			--(*left);
			ioterm = CDR(objp);
			if (!is_list(ioterm) || !(*left)) {
			    break;
			}
			objp = list_val(ioterm);
			obj = CAR(objp);
			if (!is_small(obj))
			    break;
		    }
		} else if (is_nil(obj)) {
		    ioterm = CDR(objp);
		    if (!is_list(ioterm)) {
			break;
		    }
		    objp = list_val(ioterm);
		    obj = CAR(objp);
		} else if (is_list(obj)) {
		    /* push rest of list for later processing, start 
		       again with sublist */
		    ESTACK_PUSH(stack,CDR(objp));
		    ioterm = obj;
		    goto L_Again;
		} else if (is_bitstring(obj)) {
		    Eterm rest_term;
		    rest_term = do_build_utf8(p,obj,left,latin1,target,pos, characters, err, 
					      leftover, num_leftovers);
		    if ((*err) != 0) {
			Eterm *hp;
			hp = HAlloc(p, 2);
			obj = CDR(objp);
			ioterm = CONS(hp, rest_term, obj);
			/* (*left) = 0; */
			goto done;
		    }
		    if (rest_term != NIL) {
			Eterm *hp;
			hp = HAlloc(p, 2);
			obj = CDR(objp);
			ioterm = CONS(hp, rest_term, obj);
			(*left) = 0;
			break;
		    }
		    ioterm = CDR(objp);
		    if (is_list(ioterm)) {
			/* objp and obj need to be updated if 
			   loop is to continue */
			objp = list_val(ioterm);
			obj = CAR(objp);
		    }
		} else {
		    *err = 1;
		    goto done;
		} 
		if (!(*left) || is_nil(ioterm) || !is_list(ioterm)) {
		    break;
		}
	    } /* for(;;) */
	} /* is_list(ioterm) */

	if ((*left) && !is_list(ioterm) && !is_nil(ioterm)) {
	    /* improper list end */
	    if (is_bitstring(ioterm)) {
		ioterm = do_build_utf8(p,ioterm,left,latin1,target,pos,characters,err,leftover,num_leftovers);
		if ((*err) != 0) {
		    goto done;
		}
	    } else {
		*err = 1;
		goto done;
	    }
	}
    } /* while left and not estack empty */
 done:
    c = ESTACK_COUNT(stack);
    if (c > 0) {
	Eterm *hp = HAlloc(p,2*c);
	while(!ESTACK_ISEMPTY(stack)) {
	    Eterm st = ESTACK_POP(stack);
	    ioterm = CONS(hp, ioterm, st);
	    hp += 2;
	}
    }
    DESTROY_ESTACK(stack);
    return ioterm;

}

static int check_leftovers(const byte *source, int size) 
{
    if (((*source) & ((byte) 0xE0)) == 0xC0) {
	return 0;
    } else if (((*source) & ((byte) 0xF0)) == 0xE0) {
	if (size < 2 || 
	    (size < 3 && ((source[1] & ((byte) 0xC0)) == 0x80))) { 
	    return 0;
	}
    } else if (((*source) & ((byte) 0xF8)) == 0xF0) {
	if (size < 2 ||
	    (size < 3 && ((source[1] & ((byte) 0xC0)) == 0x80)) ||
	    (size < 4 && 
	     ((source[1] & ((byte) 0xC0)) == 0x80) &&
	     ((source[2] & ((byte) 0xC0)) == 0x80))) {
	    return 0;
	}
    }
    return -1;
}


static Eterm
mk_utf8_result_bin(Process *p, Eterm bin, Uint pos)
{
#ifdef DEBUG
    if (thing_subtag(*bitstring_val(bin)) == HEAP_BITS_SUBTAG) {
        ASSERT(bitstring_size(bin) <= ERL_ONHEAP_BITS_LIMIT);
    } else {
        ASSERT(bitstring_size(bin) > ERL_ONHEAP_BITS_LIMIT);
    }
#endif

    return erts_shrink_binary_term(bin, pos);
}

static BIF_RETTYPE build_utf8_return(Process *p, Eterm bin, Uint pos,
                                     Eterm rest_term, int err,
                                     byte *leftover, int num_leftovers,
                                     Eterm latin1)
{
    Eterm result;
    Eterm *hp;

    if (err == 0 && rest_term == NIL && num_leftovers == 0) {
        /* All done. */
        result = mk_utf8_result_bin(p, bin, pos);
    } else if (err == 0 && rest_term != NIL) {
        /* Trap */
        if (num_leftovers > 0) {
            Eterm leftover_bin =
                erts_new_binary_from_data(p,
                                          num_leftovers,
                                          leftover);
            hp = HAlloc(p, 2);
            rest_term = CONS(hp, leftover_bin, rest_term);
        }

        BUMP_ALL_REDS(p);
        BIF_TRAP4(&characters_to_utf8_trap_exp,
                  p,
                  bin,
                  erts_make_integer(pos, p),
                  rest_term,
                  latin1);
    } else {
        Eterm reason = am_error;

        if (err != 0) {
            if (num_leftovers > 0) {
                Eterm leftover_bin = erts_new_binary_from_data(p,
                                                               num_leftovers,
                                                               leftover);

                hp = HAlloc(p, 4);
                rest_term = CONS(hp, rest_term, NIL);
                hp += 2;
                rest_term = CONS(hp, leftover_bin, rest_term);
                hp += 2;
            }
        } else {
            ASSERT(rest_term == NIL && num_leftovers > 0);

            rest_term = erts_new_binary_from_data(p,
                                                  num_leftovers,
                                                  leftover);

            if (check_leftovers(leftover, num_leftovers) == 0) {
                reason = am_incomplete;
            }
        }

        hp = HAlloc(p, 4);
        result = TUPLE3(hp, reason, mk_utf8_result_bin(p, bin, pos), rest_term);
    }

    BIF_RET(result);
}


static BIF_RETTYPE characters_to_utf8_trap(BIF_ALIST_4)
{
    ERTS_DECLARE_DUMMY(Uint offset);
    ERTS_DECLARE_DUMMY(Uint size);
    byte* bytes;
    Eterm rest_term;
    Sint left, sleft;
    Uint pos;
    int err;
    byte leftover[4]; /* used for temp buffer too, 
			 otherwise 3 bytes would have been enough */
    int num_leftovers = 0;
    int latin1;
    Uint characters = 0;
    
    /*erts_printf("Trap %T!\r\n",BIF_ARG_3);*/
    ASSERT(is_bitstring(BIF_ARG_1));
    ASSERT(is_integer(BIF_ARG_2));

    ERTS_GET_BITSTRING(BIF_ARG_1, bytes, offset, size);
    (void)term_to_Uint(BIF_ARG_2, &pos);
    ASSERT(offset == 0 && TAIL_BITS(size) == 0 && pos <= BYTE_SIZE(size));

    ASSERT(is_atom(BIF_ARG_4));
    latin1 = (BIF_ARG_4 == am_latin1);

    sleft = left = allowed_iterations(BIF_P);
    err = 0;

    rest_term = do_build_utf8(BIF_P, BIF_ARG_3, &left, latin1,
                              bytes, &pos, &characters,
                              &err, leftover, &num_leftovers); 
    cost_to_proc(BIF_P, sleft - left);
    return build_utf8_return(BIF_P,BIF_ARG_1,pos,rest_term,err,
                             leftover,num_leftovers, BIF_ARG_4);
}

BIF_RETTYPE unicode_bin_is_7bit_1(BIF_ALIST_1)
{
    Sint need;
    if(!is_bitstring(BIF_ARG_1)) {
	BIF_RET(am_false);
    }
    need = latin1_binary_need(BIF_ARG_1);
    if(need >= 0 && aligned_binary_size(BIF_ARG_1) == need) {
	BIF_RET(am_true);
    }
    BIF_RET(am_false);
}

static int is_valid_utf8(Eterm orig_bin)
{
    const byte *temp_alloc = NULL, *bytes;
    Uint size;

    bytes = erts_get_aligned_binary_bytes(orig_bin, &size, &temp_alloc);
    if (bytes != NULL) {
        ERTS_DECLARE_DUMMY(const byte *endpos);
        ERTS_DECLARE_DUMMY(Uint numchar);
        int ret;

        ret = erts_analyze_utf8(bytes, size, &endpos, &numchar, NULL);

        erts_free_aligned_binary_bytes(temp_alloc);
        return ret == ERTS_UTF8_OK;
    }

    return 0;
}

BIF_RETTYPE unicode_characters_to_binary_2(BIF_ALIST_2)
{
    Sint need;
    Uint characters;
    int latin1;
    Eterm bin;
    byte *bytes;
    Uint pos;
    int err;
    Sint left, sleft;
    Eterm rest_term, subject;
    byte leftover[4]; /* used for temp buffer too, o
			 therwise 3 bytes would have been enough */
    int num_leftovers = 0;
    Uint cost_of_utf8_need;


    if (BIF_ARG_2 == am_latin1) {
	latin1 = 1;
    } else if (BIF_ARG_2 == am_unicode || BIF_ARG_2 == am_utf8) {
	latin1 = 0;
    } else {
	BIF_TRAP2(c_to_b_int_trap_exportp, BIF_P, BIF_ARG_1, BIF_ARG_2);
    }	
    if (is_list(BIF_ARG_1) && is_bitstring(CAR(list_val(BIF_ARG_1))) && 
	is_nil(CDR(list_val(BIF_ARG_1)))) {
	subject = CAR(list_val(BIF_ARG_1));
    } else {
	subject = BIF_ARG_1;
    }

    need = utf8_need(subject,latin1,&cost_of_utf8_need);
    if (need < 0) {
	BIF_ERROR(BIF_P,BADARG);
    }
    if (is_bitstring(subject) && need >= 0 && aligned_binary_size(subject) == need
	&& (latin1 || is_valid_utf8(subject))) {
	cost_to_proc(BIF_P, simple_loops_to_common(cost_of_utf8_need)); 
	    BIF_RET(subject);
    }

    bin = erts_new_binary(BIF_P, need, &bytes);
    cost_to_proc(BIF_P, simple_loops_to_common(cost_of_utf8_need));
    left = allowed_iterations(BIF_P) -
        simple_loops_to_common(cost_of_utf8_need);

    /* Simplified - ensure that we will not trap when the result is a heap
     * binary. */
    if (left <= ERL_ONHEAP_BINARY_LIMIT) {
        left = ERL_ONHEAP_BINARY_LIMIT;
    }

    sleft = left;
    pos = 0;
    err = 0;

    rest_term = do_build_utf8(BIF_P, subject, &left, latin1,
			      bytes, &pos, &characters, &err, leftover, &num_leftovers); 
#ifdef HARDDEBUG
    if (left == 0) {
	Eterm bin;
	if (is_bitstring(subject)) {
	    bin = subject;
	} else if(is_list(subject) && is_bitstring(CAR(list_val(subject)))) {
	    bin = CAR(list_val(subject));
	} else {
	    bin = NIL;
	}
	if (is_bitstring(bin)) {
            byte *temp_alloc = NULL;
            Uint i, size;
            byte *data;

            data = erts_get_aligned_binary_bytes(bin, &size, &temp_alloc);
            ASSERT(data);

            erts_printf("<<");
            for (i = 0; i < sz; ++i) {
                unsigned byte = data[i];
                erts_printf((i == sz - 1) ? "0x%X" : "0x%X, ", data);
            }
            erts_printf(">>: ");

            erts_free_aligned_binary_bytes(temp_alloc);
        }
        erts_printf("%ld - %ld = %ld\n", sleft, left, sleft - left);
    }
#endif
    cost_to_proc(BIF_P, sleft - left); 
    return build_utf8_return(BIF_P,bin,pos,rest_term,err,
			     leftover,num_leftovers,BIF_ARG_2);
}

static BIF_RETTYPE build_list_return(Process *p, byte *bytes, Uint pos, Uint characters,
				     Eterm rest_term, int err,
				     byte *leftover, int num_leftovers,
				     Eterm latin1, Sint left)
{
    Eterm *hp;
    
    if (left <= 0) {
	left = 1;
    }
    
    if (err) {
        if (num_leftovers > 0) {
            Eterm leftover_bin =
                erts_new_binary_from_data(p,
                                          num_leftovers,
                                          leftover);
	    hp = HAlloc(p,4);
	    rest_term = CONS(hp,rest_term,NIL);
	    hp += 2;
	    rest_term = CONS(hp,leftover_bin,rest_term);
	}
	BIF_RET(finalize_list_to_list(p, bytes, rest_term, 0U, pos, characters, ERTS_UTF8_ERROR, left, NIL));
    } else if (rest_term == NIL && num_leftovers != 0) {
        Eterm leftover_bin =
            erts_new_binary_from_data(p,
                                      num_leftovers,
                                      leftover);

	if (check_leftovers(leftover,num_leftovers) != 0) {
	    BIF_RET(finalize_list_to_list(p, bytes, leftover_bin, 0U, pos, characters, ERTS_UTF8_ERROR, 
					  left, NIL));
	} else {
	    BIF_RET(finalize_list_to_list(p, bytes, leftover_bin, 0U, pos, characters, ERTS_UTF8_INCOMPLETE, 
					  left, NIL));
	}
    } else { /* All OK */	    
	if (rest_term != NIL) { /* Trap */
	    RestartContext rc;
	    if (num_leftovers > 0) {
                Eterm rest_bin =
                    erts_new_binary_from_data(p,
                                              num_leftovers,
                                              leftover);

		hp = HAlloc(p,2);
		rest_term = CONS(hp,rest_bin,rest_term);
	    }
	    BUMP_ALL_REDS(p);
	    rc.bytes = bytes;
	    rc.num_processed_bytes = 0; /* not used */
	    rc.num_bytes_to_process = pos;
	    rc.num_resulting_chars = characters;
	    rc.state = ERTS_UTF8_OK; /* not used */
	    BIF_TRAP3(&characters_to_list_trap_1_exp, p, make_magic_bin_for_restart(p,&rc),
		      rest_term, latin1);
	} else { /* Success */
	    BIF_RET(finalize_list_to_list(p, bytes, NIL, 0U, pos, characters, ERTS_UTF8_OK, left, NIL));
	}
    }
}

static BIF_RETTYPE characters_to_list_trap_1(BIF_ALIST_3)
{
    RestartContext *rc;
    byte* bytes;
    Uint pos;
    Uint characters;
    int err;
    Eterm rest_term;
    Sint left, sleft;

    int latin1;
    byte leftover[4]; /* used for temp buffer too, 
			 otherwise 3 bytes would have been enough */
    int num_leftovers = 0;
    

    rc = get_rc_from_bin(BIF_ARG_1);

    ASSERT(is_atom(BIF_ARG_3));
    latin1 = (BIF_ARG_3 == am_latin1);

    bytes = rc->bytes;
    rc->bytes = NULL; /* to avoid free due to later GC */
    pos = rc->num_bytes_to_process;
    characters = rc->num_resulting_chars;

    sleft = left = allowed_iterations(BIF_P);
    err = 0;

    rest_term = do_build_utf8(BIF_P, BIF_ARG_2, &left, latin1,
			      bytes, &pos, &characters, &err, leftover, &num_leftovers); 
    cost_to_proc(BIF_P, sleft - left);
    return build_list_return(BIF_P,bytes,pos,characters,rest_term,err,
			      leftover,num_leftovers,BIF_ARG_3,left);
}

BIF_RETTYPE unicode_characters_to_list_2(BIF_ALIST_2)
{
    Sint need;
    int latin1;
    Uint characters = 0;
    byte *bytes;
    Uint pos;
    int err;
    Sint left, sleft;
    Eterm rest_term;
    byte leftover[4]; /* used for temp buffer too, o
			 therwise 3 bytes would have been enough */
    int num_leftovers = 0;
    Uint cost_of_utf8_need;

    if (BIF_ARG_2 == am_latin1) {
	latin1 = 1;
    } else if (BIF_ARG_2 == am_unicode || BIF_ARG_2 == am_utf8) {
	latin1 = 0;
    } else {
	BIF_TRAP2(c_to_l_int_trap_exportp, BIF_P, BIF_ARG_1, BIF_ARG_2);
    }	
    if (is_bitstring(BIF_ARG_1) && !latin1) { /* Optimized behaviour for this case */
	    return utf8_to_list(BIF_P,BIF_ARG_1);
    }
    need = utf8_need(BIF_ARG_1,latin1,&cost_of_utf8_need);
    if (need < 0) {
	BIF_ERROR(BIF_P,BADARG);
    }
    bytes = alloc_restart(need);
    cost_to_proc(BIF_P, simple_loops_to_common(cost_of_utf8_need)); 
    left = allowed_iterations(BIF_P) - 
	simple_loops_to_common(cost_of_utf8_need);
    if (left <= 0) {
	/* simplified - let everything be setup by setting left to 1 */
	left = 1;
    }
    sleft = left;
    pos = 0;
    err = 0;


    rest_term = do_build_utf8(BIF_P, BIF_ARG_1, &left, latin1,
			      bytes, &pos, &characters, &err, leftover, &num_leftovers); 
    cost_to_proc(BIF_P, sleft - left); 
    return build_list_return(BIF_P,bytes,pos,characters,rest_term,err,
			     leftover,num_leftovers,BIF_ARG_2,left);
}


/*
 * When input to characters_to_list is a plain binary and the format is 'unicode', we do
 * a faster analyze and size count with this function.
 */
static ERTS_INLINE int
analyze_utf8(const byte *source, Uint size, const byte **err_pos, Uint *num_chars, int *left,
	     Sint *num_latin1_chars, Uint max_chars)
{
    int res = ERTS_UTF8_OK;
    Uint latin1_count;
    int is_latin1;
    Uint nchars = 0;
    *err_pos = source;
    if (num_latin1_chars) {
	is_latin1 = 1;
	latin1_count = 0;
    }
    while (size) {
	if (((*source) & ((byte) 0x80)) == 0) {
	    source++;
	    --size;
	    if (num_latin1_chars)
		latin1_count++;
	} else if (((*source) & ((byte) 0xE0)) == 0xC0) {
	    if (size < 2) {
		res = ERTS_UTF8_INCOMPLETE;
                break;
	    }
	    if (((source[1] & ((byte) 0xC0)) != 0x80) ||
		((*source) < 0xC2) /* overlong */) {
		res = ERTS_UTF8_ERROR;
                break;
	    }
	    if (num_latin1_chars) {
		latin1_count++;
		if ((source[0] & ((byte) 0xFC)) != ((byte) 0xC0))
		    is_latin1 = 0;
	    }
	    source += 2;
	    size -= 2;
	} else if (((*source) & ((byte) 0xF0)) == 0xE0) {
	    if (size < 3) {
		res = ERTS_UTF8_INCOMPLETE;
                break;
	    }
	    if (((source[1] & ((byte) 0xC0)) != 0x80) ||
		((source[2] & ((byte) 0xC0)) != 0x80) ||
		(((*source) == 0xE0) && (source[1] < 0xA0)) /* overlong */ ) {
		res = ERTS_UTF8_ERROR;
                break;
	    }
	    if ((((*source) & ((byte) 0xF)) == 0xD) && 
		((source[1] & 0x20) != 0)) {
		res = ERTS_UTF8_ERROR;
                break;
	    }
	    source += 3;
	    size -= 3;
	    if (num_latin1_chars)
		is_latin1 = 0;
	} else if (((*source) & ((byte) 0xF8)) == 0xF0) {
	    if (size < 4) {
		res = ERTS_UTF8_INCOMPLETE;
                break;
	    }
	    if (((source[1] & ((byte) 0xC0)) != 0x80) ||
		((source[2] & ((byte) 0xC0)) != 0x80) ||
		((source[3] & ((byte) 0xC0)) != 0x80) ||
		(((*source) == 0xF0) && (source[1] < 0x90)) /* overlong */) {
		res = ERTS_UTF8_ERROR;
                break;
	    }
	    if ((((*source) & ((byte)0x7)) > 0x4U) ||
		((((*source) & ((byte)0x7)) == 0x4U) && 
		 ((source[1] & ((byte)0x3F)) > 0xFU))) {
		res = ERTS_UTF8_ERROR;
                break;
	    }
	    source += 4;
	    size -= 4; 
	    if (num_latin1_chars)
		is_latin1 = 0;
	} else {
	    res = ERTS_UTF8_ERROR;
            break;
	}
	++nchars;
	*err_pos = source;
	if (max_chars && size > 0 && nchars == max_chars) {
	    res = ERTS_UTF8_OK_MAX_CHARS;
            break;
        }
	if (left && --(*left) <= 0 && size) {
	    res = ERTS_UTF8_ANALYZE_MORE;
            break;
	}
    }

    *num_chars = nchars;
    if (num_latin1_chars)
	*num_latin1_chars = is_latin1 ? latin1_count : -1;

    return res;
}

int erts_analyze_utf8(const byte *source, Uint size, 
		      const byte **err_pos, Uint *num_chars, int *left)
{
    return analyze_utf8(source, size, err_pos, num_chars, left, NULL, 0);
}

int erts_analyze_utf8_x(const byte *source, Uint size, 
			const byte **err_pos, Uint *num_chars, int *left,
			Sint *num_latin1_chars, Uint max_chars)
{
    return analyze_utf8(source, size, err_pos, num_chars, left, num_latin1_chars, max_chars);
}

Eterm
erts_make_list_from_utf8_buf(Eterm **hpp, Uint num,
                             const byte *bytes, Uint sz,
                             Uint *num_built, Uint *num_eaten,
                             Eterm tail)
{
    Eterm *hp;
    Eterm ret;
    Uint left = num;
    const byte *source, *ssource;
    Uint unipoint;
    hp = *hpp;
    ret = tail;
    source = bytes + sz;
    ssource = source;
    while(--source >= bytes) {
	if (((*source) & ((byte) 0x80)) == 0) {
	    unipoint = (Uint) *source;
	} else if (((*source) & ((byte) 0xE0)) == 0xC0) {
	    unipoint = 
		(((Uint) ((*source) & ((byte) 0x1F))) << 6) |
		((Uint) (source[1] & ((byte) 0x3F)));
	} else if (((*source) & ((byte) 0xF0)) == 0xE0) {
	    unipoint = 
		(((Uint) ((*source) & ((byte) 0xF))) << 12) |
		(((Uint) (source[1] & ((byte) 0x3F))) << 6) |
		((Uint) (source[2] & ((byte) 0x3F))); 	 	
	} else if (((*source) & ((byte) 0xF8)) == 0xF0) {
	    unipoint = 
		(((Uint) ((*source) & ((byte) 0x7))) << 18) |
		(((Uint) (source[1] & ((byte) 0x3F))) << 12) |
		(((Uint) (source[2] & ((byte) 0x3F))) << 6) |
		((Uint) (source[3] & ((byte) 0x3F))); 	 	
	} else {
	    /* ignore 2#10XXXXXX */
	    continue;
	}
	ret = CONS(hp,make_small(unipoint),ret);
	hp += 2;
	if (--left <= 0) {
	    break;
	}
    }
    *hpp = hp;
    *num_built = num; /* Always */
    *num_eaten = (ssource - source);
    return ret;
}

/*
 * No errors should be able to occur - no overlongs, no malformed, no nothing
 */
static Eterm do_utf8_to_list(Process *p, Uint num, const byte *bytes, Uint sz,
                             Uint left, Uint *num_built, Uint *num_eaten,
                             Eterm tail)
{
    Eterm *hp;

    ASSERT(num > 0);
    if (left < num) {
	if (left > 0)
	    num = left;
	else
	    num = 1;
    }
    
    hp = HAlloc(p,num * 2);

    return erts_make_list_from_utf8_buf(&hp, num, bytes, sz,
                                        num_built, num_eaten,
                                        tail);
}
Eterm erts_utf8_to_list(Process *p, Uint num, const byte *bytes, Uint sz, Uint left,
			Uint *num_built, Uint *num_eaten, Eterm tail)
{
    return do_utf8_to_list(p, num, bytes, sz, left, num_built, num_eaten, tail);
}

Uint erts_atom_to_string_length(Eterm atom)
{
    Atom *ap;

    ASSERT(is_atom(atom));
    ap = atom_tab(atom_val(atom));

    if (ap->latin1_chars >= 0)
        return (Uint) ap->len;
    else {
        const byte* err_pos;
        Uint num_chars;
        int ares =
            erts_analyze_utf8(erts_atom_get_name(ap), ap->len, &err_pos, &num_chars, NULL);
        ASSERT(ares == ERTS_UTF8_OK); (void)ares;

        return num_chars;
    }
}

Eterm erts_atom_to_string(Eterm **hpp, Eterm atom, Eterm tail)
{
    Atom *ap;

    ASSERT(is_atom(atom));
    ap = atom_tab(atom_val(atom));
    if (ap->latin1_chars >= 0)
        return buf_to_intlist(hpp, (char*)erts_atom_get_name(ap), ap->len, tail);
    else {
        Eterm res;
        const byte* err_pos;
        Uint num_chars, num_built, num_eaten;
#ifdef DEBUG
        Eterm *hp_start = *hpp;
        int ares =
#endif
            erts_analyze_utf8(erts_atom_get_name(ap), ap->len, &err_pos, &num_chars, NULL);
        ASSERT(ares == ERTS_UTF8_OK);

        res = erts_make_list_from_utf8_buf(hpp, num_chars, erts_atom_get_name(ap), ap->len,
                                           &num_built, &num_eaten, tail);

        ASSERT(num_built == num_chars);
        ASSERT(num_eaten == ap->len);
        ASSERT(*hpp - hp_start == 2*num_chars);

        return res;
    }
}

static int is_candidate(Uint cp)
{
    int index,pos;
    if (cp < 768) return 0;
    if (cp > 4023) {
	if (cp == 12441 || cp == 12442) return 1;
	return 0;
    }
    index = cp / 32 - COMP_CANDIDATE_MAP_OFFSET;
    pos = cp % 32;
    return !!(comp_candidate_map[index] & (1UL << pos));
}

static int hashsearch(int *htab, int htab_size, CompEntry *cv, Uint16 c)
{
	int bucket = c % htab_size;
	while (htab[bucket] != -1 && cv[htab[bucket]].c != c)
	    bucket = (bucket + 1) % htab_size;
	return htab[bucket];
}

#define TRANSLATE_NO 0
#define TRANSLATE_MAYBE -1

/* The s array is reversed */
static int translate(Uint16 *s, int slen, Uint16 *res)
{
    /* Go backwards through buffer and match against tree */
    int pos = 0;
    CompEntry *cv = compose_tab;
    int *hc = hash_compose_tab;
    int cvs = compose_tab_size;
    int x;
    while (pos < slen) {
	x = hashsearch(hc,cvs*HASH_SIZE_FACTOR,cv,s[pos]);
	if (x < 0) {
	    return TRANSLATE_NO;
	} 
	if (cv[x].res) {
	    *res = cv[x].res;
	    return pos;
	}
	cvs = cv[x].num_subs;
	hc = cv[x].hash;
	cv = cv[x].subs;
	++pos;
    }
    return TRANSLATE_MAYBE;
}

static void handle_first_norm(Uint16 *savepoints, int *numpointsp, Uint unipoint)
{
    /*erts_fprintf(stderr,"CP = %d, numpoints = %d\n",(int) unipoint,(int) *numpointsp);*/
    *numpointsp = 1;
    savepoints[0] = (Uint16) unipoint;
}

static void cleanup_norm(Eterm **hpp, Uint16 *savepoints, int numpoints, Eterm *retp)
{
    Eterm *hp = *hpp;
    int res,i;
    Uint16 newpoint;
    Eterm ret = *retp;
    
    ret = CONS(hp,make_small((Uint) savepoints[0]),ret);
    hp += 2;
    
    for (i = 1;i < numpoints;) {
	if(!is_candidate(savepoints[i]) || 
	   ((res = translate(savepoints+i,numpoints - i, &newpoint)) <= 0)) {
	    ret = CONS(hp,make_small((Uint) savepoints[i]),ret);
	    hp += 2;
	    ++i;
	} else {
	    ret = CONS(hp,make_small((Uint) newpoint),ret);
	    hp += 2;
	    i += res;
	}
    }
    *retp = ret;
}
    
static void handle_potential_norm(Eterm **hpp, Uint16 *savepoints, int *numpointsp, Uint unipoint, Eterm *retp)
{
    Eterm *hp = *hpp;
    int numpoints = *numpointsp;
    int res,i;
    Uint16 newpoint;
    Eterm ret = *retp;

    /* erts_fprintf(stderr,"CP = %d, numpoints = %d\n",(int) unipoint,(int) numpoints);*/
    if ((unipoint >> 16) == 0) { /* otherwise we're done here */ 
	savepoints[numpoints++] = (Uint16) unipoint;
	res = translate(savepoints,numpoints,&newpoint);
	if (res == TRANSLATE_NO) {
	    ret = CONS(hp,make_small((Uint) savepoints[0]),ret);
	    hp += 2;
	    for (i = 1;i < numpoints;) {
		if(!is_candidate(savepoints[i]) ||
		   ((res = translate(savepoints+i,numpoints - i, &newpoint)) == 0)) {
		    ret = CONS(hp,make_small((Uint) savepoints[i]),ret);
		    hp += 2;
		    ++i;
		} else if (res > 0) {
		    ret = CONS(hp,make_small((Uint) newpoint),ret);
		    hp += 2;
		    i += res;
		} else { /* res < 0 */
		    /* A "maybe", means we are not done yet */
		    int j = 0;
		    while (i < numpoints) {
			savepoints[j++] = savepoints[i++];
		    }
		    numpoints = j;
		    goto breakaway;
		}
	    }
	    numpoints = 0;
	breakaway:
	    ;
	} else if (res > 0) {
	    numpoints = 0;
	    ret = CONS(hp,make_small((Uint) newpoint),ret);
	    hp += 2;
	} /* < 0 means go on */
    } else {
	/* Unconditional rollup, this character is larger than 16 bit */
	ret = CONS(hp,make_small((Uint) savepoints[0]),ret);
	hp += 2;
	
	for (i = 1;i < numpoints;) {
	    if(!is_candidate(savepoints[i]) || 
	       ((res = translate(savepoints+i,numpoints - i, &newpoint)) <= 0)) {
		ret = CONS(hp,make_small((Uint) savepoints[i]),ret);
		hp += 2;
		++i;
	    } else {
		ret = CONS(hp,make_small((Uint) newpoint),ret);
		hp += 2;
		i += res;
	    }
	}
	ret = CONS(hp,make_small(unipoint),ret);
	hp += 2;
	numpoints = 0;
    }	
    *hpp = hp;
    *numpointsp = numpoints;
    *retp = ret;
} 

static Eterm do_utf8_to_list_normalize(Process *p, Uint num, const byte *bytes,
                                       Uint sz)
{
    const byte *source;
    Eterm *hp,*hp_end;
    Eterm ret;
    Uint unipoint;
    Uint16 savepoints[4];
    int numpoints = 0;

    if (num == 0)
	return NIL;

    ASSERT(num > 0);

    hp = HAlloc(p,num * 2); /* May be to much */
    hp_end = hp + num * 2;
    ret = NIL;
    source = bytes + sz;
    while(--source >= bytes) {
	if (((*source) & ((byte) 0x80)) == 0) {
	    unipoint = (Uint) *source;
	} else if (((*source) & ((byte) 0xE0)) == 0xC0) {
	    unipoint = 
		(((Uint) ((*source) & ((byte) 0x1F))) << 6) |
		((Uint) (source[1] & ((byte) 0x3F))); 	
	} else if (((*source) & ((byte) 0xF0)) == 0xE0) {
	    unipoint = 
		(((Uint) ((*source) & ((byte) 0xF))) << 12) |
		(((Uint) (source[1] & ((byte) 0x3F))) << 6) |
		((Uint) (source[2] & ((byte) 0x3F))); 	 	
	} else if (((*source) & ((byte) 0xF8)) == 0xF0) {
	    unipoint = 
		(((Uint) ((*source) & ((byte) 0x7))) << 18) |
		(((Uint) (source[1] & ((byte) 0x3F))) << 12) |
		(((Uint) (source[2] & ((byte) 0x3F))) << 6) |
		((Uint) (source[3] & ((byte) 0x3F))); 	 	
	} else {
	    /* ignore 2#10XXXXXX */
	    continue;
	}
	if (numpoints) {
	    handle_potential_norm(&hp,savepoints,&numpoints,unipoint,&ret);
	    continue;
	}
	/* We are not building up any normalizations yet, look that we shouldn't start... */
	if (is_candidate(unipoint)) {
	    handle_first_norm(savepoints,&numpoints,unipoint);
	    continue;
	} 
	ret = CONS(hp,make_small(unipoint),ret);
	hp += 2;
    }
    /* so, we'we looped to the beginning, do we have anything saved? */
    if (numpoints) {
	cleanup_norm(&hp,savepoints,numpoints,&ret);
    }
    if (hp_end != hp) {
	HRelease(p,hp_end,hp);
    }
    return ret;
}

/*
 * The last step of characters_to_list, build a list from the buffer 'bytes' (created in the same way
 * as for characters_to_utf8). All sizes are known in advance and most data will be held in a 
 * "magic binary" during trapping.
 */
static BIF_RETTYPE finalize_list_to_list(Process *p, 
					 byte *bytes,
					 Eterm rest,
					 Uint num_processed_bytes,
					 Uint num_bytes_to_process, 
					 Uint num_resulting_chars, 
					 int state, Sint left,
					 Eterm tail) 
{
    Uint num_built; /* characters */
    Uint num_eaten; /* bytes */
    Eterm *hp;
    Eterm converted,ret;

    if (!num_bytes_to_process) {
	converted = tail;
    } else {
	num_built = 0;
	num_eaten = 0;
	converted = do_utf8_to_list(p, num_resulting_chars,
				    bytes, num_bytes_to_process,
				    left, &num_built, &num_eaten, tail);
	cost_to_proc(p,num_built);
	
	if (num_built != num_resulting_chars) { /* work left to do */
	    RestartContext rc;

	    rc.num_resulting_chars = num_resulting_chars - num_built;
	    rc.num_bytes_to_process = num_bytes_to_process - num_eaten;
	    rc.num_processed_bytes = num_processed_bytes + num_eaten;
	    rc.state = state;
	    rc.bytes = bytes;
	    BUMP_ALL_REDS(p);
	    BIF_TRAP3(&characters_to_list_trap_2_exp, p,
		       make_magic_bin_for_restart(p, &rc), rest, converted); 
	}
    }

    /* 
     * OK, no more trapping, let's get rid of the temporary array...
     */

    free_restart(bytes);
    if (state == ERTS_UTF8_INCOMPLETE) {
	hp = HAlloc(p,4);
	ret = TUPLE3(hp,am_incomplete,converted,rest);
    } else if (state == ERTS_UTF8_ERROR) {
	hp = HAlloc(p,4);
	ret = TUPLE3(hp,am_error,converted,rest);
    } else {
	ret = converted;
    }

    BIF_RET(ret);
}
 
static BIF_RETTYPE characters_to_list_trap_2(BIF_ALIST_3)
{
    RestartContext *rc;
    byte *bytes;

    rc = get_rc_from_bin(BIF_ARG_1);

    bytes = rc->bytes;
    rc->bytes = NULL; /* Don't want this freed just yet... */
    return finalize_list_to_list(BIF_P, bytes, BIF_ARG_2, rc->num_processed_bytes,
				 rc->num_bytes_to_process, rc->num_resulting_chars,
				 rc->state, allowed_iterations(BIF_P), BIF_ARG_3);
}


/*
 * Hooks into the process of decoding a binary depending on state.
 * If last_state is ERTS_UTF8_ANALYZE_MORE, num_bytes_to_process 
 * and num_resulting_chars will grow
 * until we're done analyzing the binary. Then we'll eat 
 * the bytes to process, lowering num_bytes_to_process and num_resulting_chars,
 * while increasing num_processed_bytes until we're done. the state 
 * indicates how to return (error, incomplete or ok) in this stage.
 * note that num_processed_bytes and num_bytes_to_process will make up the 
 * length of the binary part to process, not necessarily the length of the 
 * whole binary (if there are errors or an incomplete tail).
 *
 * Analyzing happens from the beginning of the binary towards the end,
 * while result is built from the end of the analyzed/accepted part 
 * towards the beginning.
 *
 * Note that this routine is *only* called when original input was a plain utf8 binary,
 * otherwise the rest and the sizes are known in advance, so finalize_list_to_list is 
 * used to build the resulting list (no analyzing needed).
 */
static BIF_RETTYPE do_bif_utf8_to_list(Process *p, 
				       Eterm orig_bin,
				       Uint num_processed_bytes,
				       Uint num_bytes_to_process, 
				       Uint num_resulting_chars, 
				       int state,
				       Eterm tail) 
{
    int left;
    Uint size;
    Eterm converted = NIL;
    Eterm rest = NIL;
    Eterm *hp;
    Eterm ret;
    const byte *temp_alloc = NULL, *bytes;
    const byte *endpos;
    Uint numchar;

    Uint b_sz; /* size of the non analyzed tail */
    Uint num_built; /* characters */
    Uint num_eaten; /* bytes */

    bytes = erts_get_aligned_binary_bytes(orig_bin, &size, &temp_alloc);
    if (bytes == NULL) {
        converted = NIL;
        rest = orig_bin;
        goto error_return;
    }

    left = allowed_iterations(p);

    if (state == ERTS_UTF8_ANALYZE_MORE) {
	state = erts_analyze_utf8(bytes + num_bytes_to_process,
			     size - num_bytes_to_process,
			     &endpos,&numchar,&left);
	cost_to_proc(p,numchar);
	num_resulting_chars += numchar;
	num_bytes_to_process = endpos - bytes;
	if (state == ERTS_UTF8_ANALYZE_MORE) {
	    Eterm epos = erts_make_integer(num_bytes_to_process,p);
	    Eterm enumchar = erts_make_integer(num_resulting_chars,p);
	    erts_free_aligned_binary_bytes(temp_alloc);
	    BUMP_ALL_REDS(p);
	    BIF_TRAP3(&characters_to_list_trap_3_exp, p, orig_bin, epos,
		      enumchar);
	}
    } 

    /* 
     * If we're here, we have everything analyzed and are instead building 
     */


    if (!num_bytes_to_process) {
	converted = tail;
    } else {
	num_built = 0;
	num_eaten = 0;
	converted = do_utf8_to_list(p, num_resulting_chars,
				    bytes, num_bytes_to_process,
				    left, &num_built, &num_eaten, tail);
	cost_to_proc(p,num_built);
	
	if (num_built != num_resulting_chars) { /* work left to do */
	    Eterm newnum_resulting_chars = 
		erts_make_integer(num_resulting_chars - num_built,p);
	    Eterm newnum_bytes_to_process = 
		erts_make_integer(num_bytes_to_process - num_eaten,p);
	    Eterm newnum_processed_bytes = 
		erts_make_integer(num_processed_bytes + num_eaten,p);
	    Eterm traptuple;
	    hp = HAlloc(p,7);
	    traptuple = TUPLE6(hp,orig_bin,newnum_processed_bytes,
			       newnum_bytes_to_process, 
			       newnum_resulting_chars,
			       make_small(state),
			       converted);
	    BUMP_ALL_REDS(p);
	    erts_free_aligned_binary_bytes(temp_alloc);
	    BIF_TRAP1(&characters_to_list_trap_4_exp,p,traptuple);
	}
    }

    /* 
     * OK, no more trapping, let's build rest binary if there should
     * be one. 
     */

    b_sz = size - (num_bytes_to_process + num_processed_bytes);
    ASSERT((b_sz == 0) ^ (state != ERTS_UTF8_OK));

    if (b_sz) {
        Uint offset = num_bytes_to_process + num_processed_bytes;
        rest = erts_make_sub_binary(p, orig_bin, offset, b_sz);
    } 

    /* Done */

    if (state == ERTS_UTF8_INCOMPLETE) {
	if (check_leftovers(bytes + num_bytes_to_process + num_processed_bytes,
			    b_sz) != 0) {
	    goto error_return;
	}
	hp = HAlloc(p,4);
	ret = TUPLE3(hp,am_incomplete,converted,rest);
    } else if (state == ERTS_UTF8_ERROR) {
 error_return:
	hp = HAlloc(p,4);
	ret = TUPLE3(hp,am_error,converted,rest);
    } else {
	ret = converted;
    }

    erts_free_aligned_binary_bytes(temp_alloc);
    BIF_RET(ret);
}


/* 
 * This is called when there's still analyzing left to do,
 * we only reach this if original input was a binary.
 */

static BIF_RETTYPE characters_to_list_trap_3(BIF_ALIST_3)
{
    Uint num_bytes_to_process;
    Uint num_resulting_chars;

    term_to_Uint(BIF_ARG_2, &num_bytes_to_process); /* The number of already
						       analyzed and accepted 
						       bytes */
    term_to_Uint(BIF_ARG_3, &num_resulting_chars); /* The number of chars
						      procuced by the
						      already analyzed
						      part of the binary */

    /*erts_printf("Trap: %T, %T, %T\n",BIF_ARG_1, BIF_ARG_2, BIF_ARG_3);*/

    return do_bif_utf8_to_list(BIF_P, 
			       BIF_ARG_1, /* the binary */ 
			       0U, /* nothing processed yet */
			       num_bytes_to_process, 
			       num_resulting_chars,
			       ERTS_UTF8_ANALYZE_MORE, /* always this state here */
			       NIL); /* Nothing built -> no tail yet */
	
}

/*
 * This is called when analyzing is done and we are trapped during building,
 * we only reach this if original input was a binary.
 */
static BIF_RETTYPE characters_to_list_trap_4(BIF_ALIST_1)
{
    Uint num_processed_bytes;
    Uint num_bytes_to_process;
    Uint num_resulting_chars;
    Eterm orig_bin, tail;
    int last_state;
    Eterm *tplp = tuple_val(BIF_ARG_1);

    orig_bin = tplp[1];
    term_to_Uint(tplp[2], &num_processed_bytes);
    term_to_Uint(tplp[3], &num_bytes_to_process);
    term_to_Uint(tplp[4], &num_resulting_chars);
    last_state = (int) signed_val(tplp[5]);
    tail = tplp[6];

    /*erts_printf("Trap: {%T, %lu, %lu, %lu, %d, %T}\n",
      orig_bin, num_processed_bytes, num_bytes_to_process, 
      num_resulting_chars, last_state, tail);*/

    return do_bif_utf8_to_list(BIF_P, 
			       orig_bin, /* The whole binary */
			       num_processed_bytes,  /* Number of bytes 
							already processed */
			       num_bytes_to_process, /* Bytes left to proc. */
			       num_resulting_chars,  /* Num chars left to 
							build */
			       last_state,           /* The current state 
							(never ANALYZE_MORE)*/
			       tail);                /* The already built 
							tail */  
	
}
/*
 * This is only used when characters are a plain unicode (utf8) binary.
 * Instead of building an utf8 buffer, we analyze the binary given and use that.
 */

static BIF_RETTYPE utf8_to_list(Process* p, Eterm arg)
{
    if (!is_bitstring(arg) || aligned_binary_size(arg) < 0) {
	BIF_ERROR(p, BADARG);
    }
    return do_bif_utf8_to_list(p, arg, 0U, 0U, 0U,
			       ERTS_UTF8_ANALYZE_MORE, NIL);
}


BIF_RETTYPE atom_to_binary_2(BIF_ALIST_2)
{
    Atom* ap;

    if (is_not_atom(BIF_ARG_1)) {
	goto error;
    }

    ap = atom_tab(atom_val(BIF_ARG_1));

    if (BIF_ARG_2 == am_latin1) {
	Eterm bin_term;
        if (ap->latin1_chars < 0) {
            goto error;
        }

        if (ap->latin1_chars == ap->len) {
	    BIF_RET(ap->u.bin);
        } else {
            byte* bin_p;
            int dbg_sz;
            bin_term = erts_new_binary(BIF_P, ap->latin1_chars, &bin_p);
            dbg_sz = erts_utf8_to_latin1(bin_p, erts_atom_get_name(ap), ap->len);
            ASSERT(dbg_sz == ap->latin1_chars); (void)dbg_sz;
	    BIF_RET(bin_term);
        }
    } else if (BIF_ARG_2 == am_utf8 || BIF_ARG_2 == am_unicode) {
	BIF_RET(ap->u.bin);
    } else {
    error:
	BIF_ERROR(BIF_P, BADARG);
    }
}

static BIF_RETTYPE
binary_to_atom(Process* proc, Eterm bin, Eterm enc, int must_exist)
{
    const byte *temp_alloc = NULL, *bytes;
    Uint bin_size;
    Eterm a;

    bytes = erts_get_aligned_binary_bytes(bin, &bin_size, &temp_alloc);
    if (bytes == NULL) {
        BIF_ERROR(proc, BADARG);
    }

    if (enc == am_latin1) {
	if (!must_exist) {
	    int lix = erts_atom_put_index((byte *) bytes,
					  bin_size,
					  ERTS_ATOM_ENC_LATIN1,
					  0);
	    if (lix == ATOM_BAD_ENCODING_ERROR) {
	    badarg:
		erts_free_aligned_binary_bytes(temp_alloc);
		BIF_ERROR(proc, BADARG);
	    } else if (lix == ATOM_MAX_CHARS_ERROR) {
	    system_limit:
		erts_free_aligned_binary_bytes(temp_alloc);
		BIF_ERROR(proc, SYSTEM_LIMIT);
	    }

	    a = make_atom(lix);
	} else if (!erts_atom_get((char *)bytes, bin_size, &a, ERTS_ATOM_ENC_LATIN1)) {
	    goto badarg;
	}

    } else if (enc == am_utf8 || enc == am_unicode) {
	if (!must_exist) {
	    int uix = erts_atom_put_index((byte *) bytes,
					  bin_size,
					  ERTS_ATOM_ENC_UTF8,
					  0);
	    if (uix == ATOM_BAD_ENCODING_ERROR) {
		goto badarg;
	    } else if (uix == ATOM_MAX_CHARS_ERROR) {
		goto system_limit;
	    }

	    a = make_atom(uix);
	}
	else if (!erts_atom_get((char*)bytes, bin_size, &a, ERTS_ATOM_ENC_UTF8)) {
	    goto badarg;
	}
    } else {
	goto badarg;
    }

    erts_free_aligned_binary_bytes(temp_alloc);
    BIF_RET(a);
}

BIF_RETTYPE binary_to_atom_2(BIF_ALIST_2)
{
    return binary_to_atom(BIF_P, BIF_ARG_1, BIF_ARG_2, 0);
}

BIF_RETTYPE binary_to_existing_atom_2(BIF_ALIST_2)
{
    return binary_to_atom(BIF_P, BIF_ARG_1, BIF_ARG_2, 1);
}

/**********************************************************
 * Simpler non-interruptable routines for UTF-8 and 
 * Windowish UTF-16 (restricted)
 **********************************************************/
/*
 * This function is the heart of the Unicode support for 
 * open_port - spawn_executable. It converts both the name
 * of the executable and the arguments according to the same rules
 * as for filename conversion. That means as if your arguments are
 * to be raw, you supply binaries, else unicode characters are allowed up to
 * the encoding maximum (256 of the unicode max).
 * Depending on the filename encoding standard, the vector is then
 * converted to whatever is used, which might mean win_utf16 if on windows.
 * Do not peek into the argument vector or filenam with ordinary
 * string routines, that will certainly fail on some OS.
 */

char *erts_convert_filename_to_native(Eterm name, char *statbuf, size_t statbuf_size,
				      ErtsAlcType_t alloc_type, int allow_empty,
				      int allow_atom, Sint *used)
{
    int encoding = erts_get_native_filename_encoding();
    return erts_convert_filename_to_encoding(name, statbuf, statbuf_size, alloc_type,
					     allow_empty, allow_atom, encoding,
					     used, 0);
}

char *erts_convert_filename_to_encoding(Eterm name, char *statbuf, size_t statbuf_size,
					ErtsAlcType_t alloc_type, int allow_empty,
					int allow_atom, int encoding, Sint *used,
					Uint extra)
{
    char* name_buf = NULL;

    if ((allow_atom && is_atom(name)) || 
	is_list(name) || 
	(allow_empty && is_nil(name))) {
	Sint need;
	if ((need = erts_native_filename_need(name, encoding)) < 0) {
	    return NULL;
	}
	if (encoding == ERL_FILENAME_WIN_WCHAR) {
	    need += 2;
	    extra *= 2;
	} else {
	    ++need;
	}
	if (used) 
	    *used = need;
	if (need+extra > statbuf_size) {
	    name_buf = (char *) erts_alloc(alloc_type, need+extra);
	} else {
	    name_buf = statbuf;
	}
	erts_native_filename_put(name,encoding,(byte *)name_buf); 
	name_buf[need-1] = 0;
	if (encoding == ERL_FILENAME_WIN_WCHAR) {
	    name_buf[need-2] = 0;
	}
    } else if (is_bitstring(name)) {
        const byte *temp_alloc = NULL, *bytes;
        Uint size;

        bytes = erts_get_aligned_binary_bytes(name, &size, &temp_alloc);
        if (bytes == NULL) {
            return NULL;
        }

	if (encoding != ERL_FILENAME_WIN_WCHAR) {
	    /*Add 0 termination only*/
	    if (used) 
		*used = (Sint) size+1;
	    if (size+1+extra > statbuf_size) {
		name_buf = (char *) erts_alloc(alloc_type, size+1+extra);
	    } else {
		name_buf = statbuf;
	    }
	    sys_memcpy(name_buf,bytes,size);
	    name_buf[size]=0;
	} else {
            name_buf = erts_convert_filename_to_wchar(bytes, size,
                                                      statbuf, statbuf_size,
                                                      alloc_type, used, extra);
        }
	erts_free_aligned_binary_bytes(temp_alloc);
    } else {
	return NULL;
    }
    return name_buf;
}

char* erts_convert_filename_to_wchar(const byte* bytes, Uint size,
                                     char *statbuf, size_t statbuf_size,
                                     ErtsAlcType_t alloc_type, Sint* used,
                                     Uint extra_wchars)
{
    const byte *err_pos;
    Uint num_chars;
    char* name_buf = NULL;
    Sint need;
    char *p;

    if (erts_analyze_utf8(bytes,size,&err_pos,&num_chars,NULL) != ERTS_UTF8_OK ||
        erts_get_user_requested_filename_encoding() ==  ERL_FILENAME_LATIN1) {

        /* What to do now? Maybe latin1, so just take byte for byte instead */
        need = (Sint) (size + extra_wchars + 1) * 2;
        if (need > statbuf_size) {
            name_buf = (char *) erts_alloc(alloc_type, need);
        } else {
            name_buf = statbuf;
        }
        p = name_buf;
        while (size--) {
            *p++ = *bytes++;
            *p++ = 0;
        }
    } else { /* WIN_WCHAR and valid UTF8 */
        need = (Sint) (num_chars + extra_wchars + 1) * 2;
        if (need > statbuf_size) {
            name_buf = (char *) erts_alloc(alloc_type, need);
        } else {
            name_buf = statbuf;
        }
        erts_copy_utf8_to_utf16_little((byte *) name_buf, bytes, num_chars);
        p = name_buf + num_chars*2;
    }
    *p++ = 0;
    *p++ = 0;
    if (used)
        *used = p - name_buf;
    return name_buf;
}

Eterm erts_convert_native_to_filename(Process *p, size_t size, byte *bytes)
{
    Uint num_chars;
    Eterm *hp;
    const byte *err_pos;
    Uint num_built; /* characters */
    Uint num_eaten; /* bytes */
    Eterm ret;
    int mac = 0;

    switch (erts_get_native_filename_encoding()) {
    case ERL_FILENAME_LATIN1:
	goto noconvert;
    case ERL_FILENAME_UTF8_MAC:
	mac = 1;
        ERTS_FALLTHROUGH();
    case ERL_FILENAME_UTF8:
	if (size == 0)
	    return NIL;
	if (erts_analyze_utf8(bytes,size,&err_pos,&num_chars,NULL) != ERTS_UTF8_OK) {
	    goto noconvert;
	}
	num_built = 0;
	num_eaten = 0;
	if (mac) {
	    ret = do_utf8_to_list_normalize(p, num_chars, bytes, size);
	} else {
	    ret = do_utf8_to_list(p, num_chars, bytes, size, num_chars, &num_built, &num_eaten, NIL);
	} 
	return ret;
    case ERL_FILENAME_WIN_WCHAR:
	if ((size % 2) != 0) { /* Panic fixup to avoid crashing the emulator */
	    size--;
	    hp = HAlloc(p, size+2);
	    ret = CONS(hp,make_small((Uint) bytes[size]),NIL);
	    hp += 2;
	} else {
	    hp = HAlloc(p, size);
	    ret = NIL;
	}
	bytes += size-1;
	while (size > 0) {
	    Uint x = ((Uint) *bytes--) << 8;
	    x |= ((Uint) *bytes--);
	    size -= 2;
	    ret = CONS(hp,make_small(x),ret);
	    hp += 2;
	}	    
	return ret;
    default:
	goto noconvert;
    }
 noconvert:
    hp = HAlloc(p, 2 * size);
    return erts_bin_bytes_to_list(NIL, hp, bytes, size, 0);
}


Sint erts_native_filename_need(Eterm ioterm, int encoding)
{
    Eterm *objp;
    Eterm obj;
    DECLARE_ESTACK(stack);
    Sint need = 0;

    if (is_atom(ioterm)) {
	Atom* ap;
	int i;
	ap = atom_tab(atom_val(ioterm));
	switch (encoding) {
	case ERL_FILENAME_LATIN1:
	    need = ap->latin1_chars;  /* May be -1 */
	    break;
	case ERL_FILENAME_UTF8_MAC:
	case ERL_FILENAME_UTF8:
	    need = ap->len;
	    break;
	case ERL_FILENAME_WIN_WCHAR:
            if (ap->latin1_chars >= 0) {
		need = 2* ap->latin1_chars;
            }
	    else {
		const byte * name = erts_atom_get_name(ap);
		for (i = 0; i < ap->len; ) {
                    if (name[i] < 0x80) {
			i++;
                    } else if (name[i] < 0xE0) {
			i += 2;
                    } else if (name[i] < 0xF0) {
			i += 3;
                    } else {
			need = -1;
			break;
		    }
		    need += 2;
		}
	    }
	    break;
	default:
	    need = -1;
	}
        /*
         * Do not allow null in
         * the middle of filenames
         */
        if (need > 0) {
            const byte *name = erts_atom_get_name(ap);
            int len = ap->len;
            for (i = 0; i < len; i++) {
                if (name[i] == 0) {
                    need = -1;
                    break;
                }
            }
        }
	DESTROY_ESTACK(stack);
	return need;
    }

    if (is_nil(ioterm)) {
	DESTROY_ESTACK(stack);
	return need;
    }
    if (!is_list(ioterm)) {
	DESTROY_ESTACK(stack);
	return (Sint) -1;
    }
    /* OK a list, needs to be processed in order, handling each flat list-level
       as they occur, just like io_list_to_binary would */
    ESTACK_PUSH(stack,ioterm);
    while (!ESTACK_ISEMPTY(stack)) {
	ioterm = ESTACK_POP(stack);	
	if (is_nil(ioterm)) {
	    /* ignore empty lists */
	    continue;
	}
	if(is_list(ioterm)) {
L_Again:   /* Restart with sublist, old listend was pushed on stack */
	    objp = list_val(ioterm);
	    obj = CAR(objp);
	    for(;;) { /* loop over one flat list of bytes and binaries
		         until sublist or list end is encountered */
		if (is_small(obj)) { /* Always small */
		    for(;;) {
			Uint x = unsigned_val(obj);
                        /*
                         * Do not allow null in
                         * the middle of filenames
                         */
                        if (x == 0) {
                            DESTROY_ESTACK(stack);
                            return ((Sint) -1);
                        }
			switch (encoding) {
			case ERL_FILENAME_LATIN1:
			    if (x > 255) {
				DESTROY_ESTACK(stack);
				return ((Sint) -1);
			    }
			    need += 1;
			    break;
			case ERL_FILENAME_UTF8_MAC:
			case ERL_FILENAME_UTF8:
			    if (x < 0x80) {
				need +=1;
			    } else if (x < 0x800) {
				need += 2;
			    } else if (x < 0x10000) {
				if (x >= 0xD800 && x <= 0xDFFF) {
				    /* Invalid unicode range */
				    DESTROY_ESTACK(stack);
				    return ((Sint) -1);
				}
				need += 3;
			    } else  if (x < 0x110000) {
				need += 4; 
			    } else {
				DESTROY_ESTACK(stack);
				return ((Sint) -1);
			    }
			    break;
			case ERL_FILENAME_WIN_WCHAR:
			    if (x <= 0xffff) { 
				need += 2;
				break;
			    } /* else fall through to error */
                            ERTS_FALLTHROUGH();
			default:
			    DESTROY_ESTACK(stack);
			    return ((Sint) -1);
			}
			    
			/* everything else will give badarg later 
			   in the process, so we dont check */
			ioterm = CDR(objp);
			if (!is_list(ioterm)) {
			    break;
			}
			objp = list_val(ioterm);
			obj = CAR(objp);
			if (!is_small(obj))
			    break;
		    }
		} else if (is_nil(obj)) {
		    ioterm = CDR(objp);
		    if (!is_list(ioterm)) {
			break;
		    }
		    objp = list_val(ioterm);
		    obj = CAR(objp);
		} else if (is_list(obj)) {
		    /* push rest of list for later processing, start 
		       again with sublist */
		    ESTACK_PUSH(stack,CDR(objp));
		    ioterm = obj;
		    goto L_Again;
		} else {
		    DESTROY_ESTACK(stack);
		    return ((Sint) -1);
		} 
		if (is_nil(ioterm) || !is_list(ioterm)) {
		    break;
		}
	    } /* for(;;) */
	} /* is_list(ioterm) */
	
	if (!is_list(ioterm) && !is_nil(ioterm)) {
	    /* improper list end */
	    DESTROY_ESTACK(stack);
	    return ((Sint) -1);
	}
    } /* while  not estack empty */
    DESTROY_ESTACK(stack);
    return need;
}

void erts_native_filename_put(Eterm ioterm, int encoding, byte *p) 
{
    Eterm *objp;
    Eterm obj;
    DECLARE_ESTACK(stack);

    if (is_atom(ioterm)) {
	Atom* ap;
	int i;
	ap = atom_tab(atom_val(ioterm));
	switch (encoding) {
	case ERL_FILENAME_LATIN1:
	    for (i = 0; i < ap->len; i++) {
		if (erts_atom_get_name(ap)[i] < 0x80) {
		    *p++ = erts_atom_get_name(ap)[i];
		} else {
		    ASSERT(erts_atom_get_name(ap)[i] < 0xC4);
		    *p++ = ((erts_atom_get_name(ap)[i] & 3) << 6) | (erts_atom_get_name(ap)[i+1] & 0x3F);
		    i++;
		}
	    }
	    break;
	case ERL_FILENAME_UTF8_MAC:
	case ERL_FILENAME_UTF8:
	    sys_memcpy(p, erts_atom_get_name(ap), ap->len);
	    break;
	case ERL_FILENAME_WIN_WCHAR:
	    for (i = 0; i < ap->len; i++) {
		/* Little endian */
                if (erts_atom_get_name(ap)[i] < 0x80) {
		    *p++ = erts_atom_get_name(ap)[i];
		    *p++ = 0;
                } else if (erts_atom_get_name(ap)[i] < 0xE0) {
		    *p++ = ((erts_atom_get_name(ap)[i] & 3) << 6) | (erts_atom_get_name(ap)[i+1] & 0x3F);
		    *p++ = ((erts_atom_get_name(ap)[i] & 0x1C) >> 2);
		    i++;
                } else {
		    ASSERT(erts_atom_get_name(ap)[i] < 0xF0);
		    *p++ = ((erts_atom_get_name(ap)[i+1] & 3) << 6) | (erts_atom_get_name(ap)[i+2] & 0x3C);
		    *p++ = ((erts_atom_get_name(ap)[i] & 0xF) << 4) | ((erts_atom_get_name(ap)[i+1] & 0x3C) >> 2);
		    i += 2;
		}
            }
	    break;
	default:
	    ASSERT(0);
	}
	DESTROY_ESTACK(stack);
	return;
    }

    if (is_nil(ioterm)) {
	DESTROY_ESTACK(stack);
	return;
    }
    ASSERT(is_list(ioterm));
    /* OK a list, needs to be processed in order, handling each flat list-level
       as they occur, just like io_list_to_binary would */
    ESTACK_PUSH(stack,ioterm);
    while (!ESTACK_ISEMPTY(stack)) {
	ioterm = ESTACK_POP(stack);	
	if (is_nil(ioterm)) {
	    /* ignore empty lists */
	    continue;
	}
	if(is_list(ioterm)) {
L_Again:   /* Restart with sublist, old listend was pushed on stack */
	    objp = list_val(ioterm);
	    obj = CAR(objp);
	    for(;;) { /* loop over one flat list of bytes and binaries
		         until sublist or list end is encountered */
		if (is_small(obj)) { /* Always small */
		    for(;;) {
			Uint x = unsigned_val(obj);
			switch (encoding) {
			case ERL_FILENAME_LATIN1:
			    ASSERT( x < 256);
			    *p++ = (byte) x;
			    break;
			case ERL_FILENAME_UTF8_MAC:
			case ERL_FILENAME_UTF8:
			    if (x < 0x80) {
				*p++ = (byte) x;
			    }
			    else if (x < 0x800) {
				*p++ = (((byte) (x >> 6)) | 
					((byte) 0xC0));
				*p++ = (((byte) (x & 0x3F)) | 
					((byte) 0x80));
			    } else if (x < 0x10000) {
				ASSERT(!(x >= 0xD800 && x <= 0xDFFF));
				*p++ = (((byte) (x >> 12)) | 
					((byte) 0xE0));
				*p++ = ((((byte) (x >> 6)) & 0x3F)  | 
					((byte) 0x80));
				*p++ = (((byte) (x & 0x3F)) | 
					((byte) 0x80));
			    } else {
				ASSERT(x < 0x110000);
				*p++ = (((byte) (x >> 18)) | 
					((byte) 0xF0));
				*p++ = ((((byte) (x >> 12)) & 0x3F)  | 
					((byte) 0x80));
				*p++ = ((((byte) (x >> 6)) & 0x3F)  | 
					((byte) 0x80));
				*p++ = (((byte) (x & 0x3F)) | 
					((byte) 0x80));
			    }
			    break;
			case ERL_FILENAME_WIN_WCHAR:
			    ASSERT(x <= 0xFFFF); 
			    *p++ = (byte) (x & 0xFFU);
			    *p++ = (byte) ((x >> 8) & 0xFFU);
			    break;
			default:
			    ASSERT(0);
			}
			    
			/* everything else will give badarg later 
			   in the process, so we dont check */
			ioterm = CDR(objp);
			if (!is_list(ioterm)) {
			    break;
			}
			objp = list_val(ioterm);
			obj = CAR(objp);
			if (!is_small(obj))
			    break;
		    }
		} else if (is_nil(obj)) {
		    ioterm = CDR(objp);
		    if (!is_list(ioterm)) {
			break;
		    }
		    objp = list_val(ioterm);
		    obj = CAR(objp);
		} else if (is_list(obj)) {
		    /* push rest of list for later processing, start 
		       again with sublist */
		    ESTACK_PUSH(stack,CDR(objp));
		    ioterm = obj;
		    goto L_Again;
		} else {
		    ASSERT(0);
		} 
		if (is_nil(ioterm) || !is_list(ioterm)) {
		    break;
		}
	    } /* for(;;) */
	} /* is_list(ioterm) */
	
	ASSERT(is_list(ioterm) || is_nil(ioterm));
    } /* while  not estack empty */
    DESTROY_ESTACK(stack);
    return;
}
void erts_copy_utf8_to_utf16_little(byte *target,
                                    const byte *bytes,
                                    Uint num_chars)
{
    Uint unipoint;
    
    while (num_chars--) {
	if (((*bytes) & ((byte) 0x80)) == 0) {
	    unipoint = (Uint) *bytes;
	    ++bytes;
	} else if (((*bytes) & ((byte) 0xE0)) == 0xC0) {
	    unipoint = 
		(((Uint) ((*bytes) & ((byte) 0x1F))) << 6) |
		((Uint) (bytes[1] & ((byte) 0x3F))); 	
	    bytes += 2;
	} else if (((*bytes) & ((byte) 0xF0)) == 0xE0) {
	    unipoint = 
		(((Uint) ((*bytes) & ((byte) 0xF))) << 12) |
		(((Uint) (bytes[1] & ((byte) 0x3F))) << 6) |
		((Uint) (bytes[2] & ((byte) 0x3F)));
	    bytes +=3;
	} else if (((*bytes) & ((byte) 0xF8)) == 0xF0) {
	    unipoint = 
		(((Uint) ((*bytes) & ((byte) 0x7))) << 18) |
		(((Uint) (bytes[1] & ((byte) 0x3F))) << 12) |
		(((Uint) (bytes[2] & ((byte) 0x3F))) << 6) |
		((Uint) (bytes[3] & ((byte) 0x3F)));
	    bytes += 4;
	} else {
	    erts_exit(ERTS_ERROR_EXIT,"Internal unicode error in prim_file:internal_name2native/1");
	}
	*target++ = (byte) (unipoint & 0xFF);
	*target++ = (byte) ((unipoint >> 8) & 0xFF);
    }
}

/*
 * *** Requirements on Raw Filename Format ***
 *
 * These requirements are due to the 'filename' module
 * in stdlib. This since it is documented that it
 * should be able to operate on raw filenames as well
 * as ordinary filenames.
 *
 * A raw filename *must* be a byte sequence where:
 * 1. Codepoints 0-127 (7-bit ascii) *must* be encoded
 *    as a byte with the corresponding value. That is,
 *    the most significant bit in the byte encoding the
 *    codepoint is never set.
 * 2. Codepoints greater than 127 *must* be encoded
 *    with the most significant bit set in *every* byte
 *    encoding it.
 *
 * Latin1 and UTF-8 meet these requirements while
 * UTF-16 and UTF-32 don't.
 *
 * On Windows filenames are natively stored as malformed
 * UTF-16LE (lonely surrogates may appear). A more correct
 * description than UTF-16 would be an array of 16-bit
 * words... In order to meet the requirements of the
 * raw file format we convert the malformed UTF-16LE to
 * malformed UTF-8 which meet the requirements.
 *
 * Note that these requirements are today only OTP
 * internal (erts-stdlib internal) requirements that
 * could be changed.
 */

/*
 * This internal bif converts a filename to whatever format is suitable for the file driver
 * It also adds zero termination so that prim_file needn't bother with the character encoding
 * of the file driver 
 */
BIF_RETTYPE prim_file_internal_name2native_1(BIF_ALIST_1)
{
    int encoding = erts_get_native_filename_encoding();
    Sint need;
    Eterm bin_term;
    byte* bin_p;

    /*
     * See comment on "Requirements on Raw Filename Format"
     * above.
     */

    /* Prim file explicitly does not allow atoms, although we could 
       very well cope with it. Instead of letting 'file' handle them,
       it would probably be more efficient to handle them here. Subject to 
       change in R15. */ 
    if (is_atom(BIF_ARG_1)) {
	BIF_ERROR(BIF_P,BADARG);
    }
    if (is_bitstring(BIF_ARG_1)) {
        const byte *temp_alloc = NULL, *bytes;
        const byte *err_pos;
        Uint size, num_chars;

        /* Uninterpreted encoding except if windows widechar, in case we
         * convert from utf8 to win_wchar */
        bytes = erts_get_aligned_binary_bytes(BIF_ARG_1, &size, &temp_alloc);
        if (bytes == NULL) {
            BIF_ERROR(BIF_P, BADARG);
        }

        if (encoding != ERL_FILENAME_WIN_WCHAR) {
            Uint i;
            /*Add 0 termination only*/
            bin_term = erts_new_binary(BIF_P, size+1, &bin_p);

            for (i = 0; i < size; i++) {
                /* Don't allow null in the middle of filenames... */
                if (bytes[i] == 0)
                    goto bin_name_error;
                bin_p[i] = bytes[i];
            }

            bin_p[size]='\0';
            erts_free_aligned_binary_bytes(temp_alloc);
            BIF_RET(bin_term);
        }

        /* In a wchar world, the emulator flags only affect how
         * binaries are interpreted when sent from the user.
         *
         * Determine real length and create a new binary */
        if (erts_analyze_utf8(bytes,size,&err_pos,&num_chars,NULL) != ERTS_UTF8_OK || 
            erts_get_user_requested_filename_encoding() ==  ERL_FILENAME_LATIN1) {
            /* What to do now? Maybe latin1, so just take byte for byte instead */
            bin_term = erts_new_binary(BIF_P, (size+1)*2, &bin_p);

	    while (size--) {
                /* Don't allow null in the middle of filenames... */
                if (*bytes == 0)
                    goto bin_name_error;
		*bin_p++ = *bytes++;
		*bin_p++ = 0;
	    }
	    *bin_p++ = 0;
	    *bin_p++ = 0;
	    erts_free_aligned_binary_bytes(temp_alloc);
	    BIF_RET(bin_term);
	}

        /* OK, UTF8 ok, number of characters is in num_chars */
        bin_term = erts_new_binary(BIF_P, (num_chars+1)*2, &bin_p);
        erts_copy_utf8_to_utf16_little(bin_p, bytes, num_chars);

        /* zero termination */
        bin_p[num_chars*2] = 0;
        bin_p[num_chars*2+1] = 0;

        erts_free_aligned_binary_bytes(temp_alloc);
        BIF_RET(bin_term);
    bin_name_error:
        erts_free_aligned_binary_bytes(temp_alloc);
        BIF_ERROR(BIF_P,BADARG);
    } /* binary */   
	    

    if ((need = erts_native_filename_need(BIF_ARG_1, encoding)) < 0) {
        BIF_ERROR(BIF_P,BADARG);
    }
    if (encoding == ERL_FILENAME_WIN_WCHAR) {
	need += 2;
    } else {
	++need;
    }

    bin_term = erts_new_binary(BIF_P, need, &bin_p);
    erts_native_filename_put(BIF_ARG_1, encoding, bin_p);

    bin_p[need-1] = 0;
    if (encoding == ERL_FILENAME_WIN_WCHAR) {
        bin_p[need-2] = 0;
    }

    BIF_RET(bin_term);
}

BIF_RETTYPE prim_file_internal_native2name_1(BIF_ALIST_1)
{
    Uint size,num_chars;
    Eterm *hp;
    const byte *temp_alloc = NULL, *bytes;
    const byte *err_pos;
    Uint num_built; /* characters */
    Uint num_eaten; /* bytes */
    Eterm ret;
    int mac = 0;

    /*
     * See comment on "Requirements on Raw Filename Format"
     * above.
     */
    bytes = erts_get_aligned_binary_bytes(BIF_ARG_1, &size, &temp_alloc);
    if (bytes == NULL) {
        BIF_ERROR(BIF_P, BADARG);
    } else if (size == 0) {
        erts_free_aligned_binary_bytes(temp_alloc);
        BIF_RET(NIL);
    }

    switch (erts_get_native_filename_encoding()) {
    case ERL_FILENAME_LATIN1:
        {
            Eterm result;

            hp = HAlloc(BIF_P, 2 * size);

            result = erts_bin_bytes_to_list(NIL, hp, bytes, size, 0);
            erts_free_aligned_binary_bytes(temp_alloc);

            BIF_RET(result);
        }
    case ERL_FILENAME_UTF8_MAC:
        mac = 1;
        /* !! FALL THROUGH !! */
    case ERL_FILENAME_UTF8:
	if (erts_analyze_utf8(bytes,size,&err_pos,&num_chars,NULL) != ERTS_UTF8_OK) {
	    Eterm *hp = HAlloc(BIF_P,3);
	    Eterm warn_type = NIL;
	    erts_free_aligned_binary_bytes(temp_alloc);
	    switch (erts_get_filename_warning_type()) {
	    case ERL_FILENAME_WARNING_IGNORE:
		warn_type = am_ignore;
		break;
	    case ERL_FILENAME_WARNING_ERROR:
		warn_type = am_error;
		break;
	    default:
		warn_type = am_warning;
	    }
	    BIF_RET(TUPLE2(hp,am_error,warn_type));
	}
	num_built = 0;
	num_eaten = 0;
	if (mac) {
	    ret = do_utf8_to_list_normalize(BIF_P, num_chars, bytes, size);
	} else {
	    ret = do_utf8_to_list(BIF_P, num_chars, bytes, size, num_chars, &num_built, &num_eaten, NIL);
	}
	erts_free_aligned_binary_bytes(temp_alloc);
	BIF_RET(ret);
    case ERL_FILENAME_WIN_WCHAR:
	if ((size % 2) != 0) { /* Panic fixup to avoid crashing the emulator */
	    size--;
	    hp = HAlloc(BIF_P, size+2);
	    ret = CONS(hp,make_small((Uint) bytes[size]),NIL);
	    hp += 2;
	} else {
	    hp = HAlloc(BIF_P, size);
	    ret = NIL;
	}
	bytes += size-1;
	while (size > 0) {
	    Uint x = ((Uint) *bytes--) << 8;
	    x |= ((Uint) *bytes--);
	    size -= 2;
	    ret = CONS(hp,make_small(x),ret);
	    hp += 2;
	}
	erts_free_aligned_binary_bytes(temp_alloc);
	BIF_RET(ret);
    default:
	break;
    }

    BIF_RET(BIF_ARG_1);
}

BIF_RETTYPE prim_file_internal_normalize_utf8_1(BIF_ALIST_1)
{
    const byte *temp_alloc = NULL, *bytes;
    Uint size;
    Eterm ret;

    ERTS_BIF_PREP_ERROR(ret, BIF_P, BADARG);

    bytes = erts_get_aligned_binary_bytes(BIF_ARG_1, &size, &temp_alloc);
    if (bytes != NULL) {
        const byte *err_pos;
        Uint num_chars;

        if (erts_analyze_utf8(bytes,size,
                              &err_pos,
                              &num_chars,
                              NULL) == ERTS_UTF8_OK) {
            ret = do_utf8_to_list_normalize(BIF_P,
                                            num_chars,
                                            bytes,
                                            size);
        }

        erts_free_aligned_binary_bytes(temp_alloc);
    }

    BIF_RET(ret);
}

BIF_RETTYPE prim_file_is_translatable_1(BIF_ALIST_1)
{
    const byte *temp_alloc = NULL, *bytes;
    int translatable = 0;
    Uint size;

    bytes = erts_get_aligned_binary_bytes(BIF_ARG_1, &size, &temp_alloc);
    if (bytes == NULL) {
        BIF_ERROR(BIF_P,BADARG);
    } else if (size == 0) {
        translatable = 1;
    } else {
        /* If the encoding is latin1, the pathname is always translatable. */
        switch (erts_get_native_filename_encoding()) {
        case ERL_FILENAME_LATIN1:
            translatable = 1;
            break;
        case ERL_FILENAME_WIN_WCHAR:
            if (erts_get_user_requested_filename_encoding() == ERL_FILENAME_LATIN1) {
                translatable = 1;
                break;
            }
            /* !! FALL THROUGH !! */
        case ERL_FILENAME_UTF8_MAC:
        case ERL_FILENAME_UTF8:
            {
                /* Check whether the binary contains legal UTF-8 sequences. */
                const byte *err_pos;
                Uint num_chars;

                translatable = (erts_analyze_utf8(bytes,
                                                  size,
                                                  &err_pos,
                                                  &num_chars,
                                                  NULL) == ERTS_UTF8_OK);
                break;
            }
        }
    }

    erts_free_aligned_binary_bytes(temp_alloc);
    BIF_RET(translatable ? am_true : am_false);
}

BIF_RETTYPE file_native_name_encoding_0(BIF_ALIST_0)
{
    switch (erts_get_native_filename_encoding()) {
    case ERL_FILENAME_LATIN1:
	BIF_RET(am_latin1);
    case ERL_FILENAME_UTF8_MAC:
    case ERL_FILENAME_UTF8:
	BIF_RET(am_utf8);
    case ERL_FILENAME_WIN_WCHAR:
	if (erts_get_user_requested_filename_encoding() ==  ERL_FILENAME_LATIN1) {
	    BIF_RET(am_latin1);
	} else {
	    BIF_RET(am_utf8);
	}
    default:
	BIF_RET(am_undefined);
    }
}

int erts_utf8_to_latin1(byte* dest, const byte* source, int slen)
{
    /*
     * Assumes source contains valid utf8 that can be encoded as latin1,
     * and that dest has enough room.
     */
    byte* dp = dest;

    while (slen > 0) {
	if ((source[0] & 0x80) == 0) {
	    *dp++ = *source++;
	    --slen;
	}
	else {
	    ASSERT(slen > 1);
	    ASSERT((source[0] & 0xFE) == 0xC2);
	    ASSERT((source[1] & 0xC0) == 0x80);
	    *dp++ = (char) ((source[0] << 6) | (source[1] & 0x3F));
	    source += 2;
	    slen -= 2;
	}
    }
    return dp - dest;
}

BIF_RETTYPE io_printable_range_0(BIF_ALIST_0)
{
    if (erts_get_printable_characters() == ERL_PRINTABLE_CHARACTERS_UNICODE) {
	BIF_RET(am_unicode);
    } else {
	BIF_RET(am_latin1);
    }
}
