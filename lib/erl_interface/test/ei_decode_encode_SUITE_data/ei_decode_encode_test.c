/*
 * %CopyrightBegin%
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright Ericsson AB 2004-2025. All Rights Reserved.
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

#include "ei_runner.h"
#include <string.h>

/*
 * Purpose: Read pids, funs and others without real meaning on the C side 
 *          and pass it back to Erlang to test that it is still the same. 
 * Author:  kent@erix.ericsson.se
 */

/*#define MESSAGE(FMT,A1,A2) message(FMT,A1,A2)*/
#define MESSAGE(FMT,A1,A2)


typedef struct
{
    char name[MAXATOMLEN_UTF8];
    erlang_char_encoding enc;
}my_atom;

typedef struct
{
    const char* bytes;
    unsigned int bitoffs;
    size_t nbits;
}my_bitstring;

struct my_obj {
    union {
	erlang_fun fun;
	erlang_pid pid;
	erlang_port port;
	erlang_ref ref;
	erlang_trace trace;
	erlang_big big;
	my_atom atom;
        my_bitstring bits;

	int arity;
    }u;

    int nterms; /* 0 for non-containers */
    char* startp; /* container start position in decode buffer */
};

typedef int decodeFT(const char *buf, int *index, struct my_obj*);
typedef int encodeFT(char *buf, int *index, struct my_obj*);
typedef int x_encodeFT(ei_x_buff*, struct my_obj*);

struct Type {
    char* name;
    char* type;
    decodeFT* ei_decode_fp;
    encodeFT* ei_encode_fp;
    x_encodeFT* ei_x_encode_fp;
};


struct Type fun_type = {
    "fun", "erlang_fun", (decodeFT*)ei_decode_fun,
    (encodeFT*)ei_encode_fun, (x_encodeFT*)ei_x_encode_fun
};

int ei_decode_my_pid(const char *buf, int *index, struct my_obj* obj)
{
    int ix = *index;
    int type = -1;
    int size = -2;
    if (ei_get_type(buf, &ix, &type, &size) != 0
        || ix != *index || type != ERL_PID_EXT || size != 0) {
        fail2("ei_get_type failed for pid, type=%d size=%d", type, size);
    }
    return ei_decode_pid(buf, index, (erlang_pid*)obj);
}

struct Type pid_type = {
    "pid", "erlang_pid", ei_decode_my_pid,
    (encodeFT*)ei_encode_pid, (x_encodeFT*)ei_x_encode_pid
};

int ei_decode_my_port(const char *buf, int *index, struct my_obj* obj)
{
    int ix = *index;
    int type = -1;
    int size = -2;
    if (ei_get_type(buf, &ix, &type, &size) != 0
        || ix != *index || type != ERL_PORT_EXT || size != 0) {
        fail2("ei_get_type failed for port, type=%d size=%d", type, size);
    }
    return ei_decode_port(buf, index, (erlang_port*)obj);
}

struct Type port_type = {
    "port", "erlang_port", ei_decode_my_port,
    (encodeFT*)ei_encode_port, (x_encodeFT*)ei_x_encode_port
};

int ei_decode_my_ref(const char *buf, int *index, struct my_obj* obj)
{
    int ix = *index;
    int type = -1;
    int size = -2;
    if (ei_get_type(buf, &ix, &type, &size) != 0
        || ix != *index || type != ERL_NEW_REFERENCE_EXT || size != 0) {
        fail2("ei_get_type failed for ref, type=%d size=%d", type, size);
    }
    return ei_decode_ref(buf, index, (erlang_ref*)obj);
}

struct Type ref_type = {
    "ref", "erlang_ref", (decodeFT*)ei_decode_my_ref,
    (encodeFT*)ei_encode_ref, (x_encodeFT*)ei_x_encode_ref
};

struct Type trace_type = {
    "trace", "erlang_trace", (decodeFT*)ei_decode_trace,
    (encodeFT*)ei_encode_trace, (x_encodeFT*)ei_x_encode_trace
};

struct Type big_type = {
    "big", "erlang_big", (decodeFT*)ei_decode_big,
    (encodeFT*)ei_encode_big, (x_encodeFT*)ei_x_encode_big
};

int ei_decode_my_atom(const char *buf, int *index, my_atom* a)
{
    return ei_decode_atom_as(buf, index, (a ? a->name : NULL), sizeof(a->name),
			     ERLANG_UTF8, (a ? &a->enc : NULL), NULL);
}
int ei_encode_my_atom(char *buf, int *index, my_atom* a)
{
    return ei_encode_atom_as(buf, index, a->name, ERLANG_UTF8, a->enc);
}
int ei_x_encode_my_atom(ei_x_buff* x, my_atom* a)
{
    return ei_x_encode_atom_as(x, a->name, ERLANG_UTF8, a->enc);
}

struct Type my_atom_type = {
    "atom", "my_atom", (decodeFT*)ei_decode_my_atom,
    (encodeFT*)ei_encode_my_atom, (x_encodeFT*)ei_x_encode_my_atom
};

int ei_decode_my_bits(const char *buf, int *index, my_bitstring* a)
{
    return ei_decode_bitstring(buf, index, (a ? &a->bytes : NULL),
                               (a ? &a->bitoffs : NULL),
                               (a ? &a->nbits : NULL));
}
int ei_encode_my_bits(char *buf, int *index, my_bitstring* a)
{
    return ei_encode_bitstring(buf, index, a->bytes, a->bitoffs, a->nbits);
}
int ei_x_encode_my_bits(ei_x_buff* x, my_bitstring* a)
{
    return ei_x_encode_bitstring(x, a->bytes, a->bitoffs, a->nbits);
}

struct Type my_bitstring_type = {
    "bits", "my_bitstring", (decodeFT*)ei_decode_my_bits,
    (encodeFT*)ei_encode_my_bits, (x_encodeFT*)ei_x_encode_my_bits
};


int my_decode_tuple_header(const char *buf, int *index, struct my_obj* obj)
{
    int ret = ei_decode_tuple_header(buf, index, &obj->u.arity);
    if (ret == 0 && obj)
	obj->nterms = obj->u.arity;
    return ret;
}

int my_encode_tuple_header(char *buf, int *index, struct my_obj* obj)
{
    return ei_encode_tuple_header(buf, index, obj->u.arity);
}
int my_x_encode_tuple_header(ei_x_buff* x, struct my_obj* obj)
{
    return ei_x_encode_tuple_header(x, (long)obj->u.arity);
}

struct Type tuple_type = {
    "tuple_header", "arity", my_decode_tuple_header,
    my_encode_tuple_header, my_x_encode_tuple_header
};


int my_decode_list_header(const char *buf, int *index, struct my_obj* obj)
{
    int ret = ei_decode_list_header(buf, index, &obj->u.arity);
    if (ret == 0 && obj) {
	obj->nterms = obj->u.arity + 1;
    }
    return ret;
}
int my_encode_list_header(char *buf, int *index, struct my_obj* obj)
{
    return ei_encode_list_header(buf, index, obj->u.arity);
}
int my_x_encode_list_header(ei_x_buff* x, struct my_obj* obj)
{
    return ei_x_encode_list_header(x, (long)obj->u.arity);
}

struct Type list_type = {
    "list_header", "arity", my_decode_list_header,
    my_encode_list_header, my_x_encode_list_header
};


int my_decode_nil(const char *buf, int *index, struct my_obj* dummy)
{
    int type, size, ret;
    ret = ei_get_type(buf, index, &type, &size);
    (*index)++;
    return ret ?  ret : !(type == ERL_NIL_EXT);

}
int my_encode_nil(char *buf, int *index, struct my_obj* dummy)
{
    return ei_encode_empty_list(buf, index);
}

int my_x_encode_nil(ei_x_buff* x, struct my_obj* dummy)
{
    return ei_x_encode_empty_list(x);
}

struct Type nil_type = {
    "empty_list", "nil", my_decode_nil,
    my_encode_nil, my_x_encode_nil
};

int my_decode_map_header(const char *buf, int *index, struct my_obj* obj)
{
    int ret = ei_decode_map_header(buf, index, &obj->u.arity);
    if (ret == 0 && obj)
	obj->nterms = obj->u.arity * 2;
    return ret;
}
int my_encode_map_header(char *buf, int *index, struct my_obj* obj)
{
    return ei_encode_map_header(buf, index, obj->u.arity);
}
int my_x_encode_map_header(ei_x_buff* x, struct my_obj* obj)
{
    return ei_x_encode_map_header(x, (long)obj->u.arity);
}

struct Type map_type = {
    "map_header", "arity", my_decode_map_header,
    my_encode_map_header, my_x_encode_map_header
};


#define BUFSZ 2000

void decode_encode(struct Type** tv, int nobj)
{
    struct my_obj objv[10];
    int oix = 0;
    char* packet;
    char* inp;
    char* outp;
    char out_buf[BUFSZ];
    int size1, size2, size3;
    int err, i;
    ei_x_buff arg;
    
    packet = read_packet(NULL);
    inp = packet+1;
    outp = out_buf;
    ei_x_new(&arg);
    for (i=0; i<nobj; i++) {
	struct Type* t = tv[i];
        int small_port = 0;

	MESSAGE("ei_decode_%s, arg is type %s", t->name, t->type);

	size1 = 0;
	err = t->ei_decode_fp(inp, &size1, NULL);
	if (err != 0) {
            fail2("decode '%s' returned non zero %d", t->name, err);
	    return;
	}
	if (size1 < 1) {
	    fail("size is < 1");
	    return;
	}

	if (size1 > BUFSZ) {
	    fail("size is > BUFSZ");
	    return;
	}

	size2 = 0;
	objv[oix].nterms = 0;
	objv[oix].startp = inp;
	err = t->ei_decode_fp(inp, &size2, &objv[oix]);
	if (err != 0) {
	    if (err != -1) {
		fail("decode returned non zero but not -1");
	    } else {
		fail("decode returned non zero");
	    }
	    return;
	}
	if (size1 != size2) {
	    MESSAGE("size1 = %d, size2 = %d\n",size1,size2);
	    fail("decode sizes differs");
	    return;
	}

	if (!objv[oix].nterms) {
	    size2 = 0;
	    err = ei_skip_term(inp, &size2);
	    if (err != 0) {
		fail("ei_skip_term returned non zero");
		return;
	    }
	    if (size1 != size2) {
		MESSAGE("size1 = %d, size2 = %d\n",size1,size2);
		fail("skip size differs");
		return;
	    }
	}

	MESSAGE("ei_encode_%s buf is NULL, arg is type %s", t->name, t->type);
	size2 = 0;
	err = t->ei_encode_fp(NULL, &size2, &objv[oix]);
	if (err != 0) {
	    if (err != -1) {
		fail("size calculation returned non zero but not -1");
		return;
	    } else {
		fail("size calculation returned non zero");
		return;
	    }
	}
	if (size1 != size2) {
            if (strcmp(t->type, "erlang_port") == 0
                && size1 == size2 + 4
                && objv[oix].u.port.id <= 0x0fffffff /* 28 bits */) {
                /* old encoding... */
                small_port = !0;
            }
            else {
                MESSAGE("size1 = %d, size2 = %d\n",size1,size2);
                fail("decode and encode size differs when buf is NULL");
                return;
            }
	}
	MESSAGE("ei_encode_%s, arg is type %s", t->name, t->type);
	size3 = 0;
	err = t->ei_encode_fp(outp, &size3, &objv[oix]);
	if (err != 0) {
	    if (err != -1) {
		fail("returned non zero but not -1");
	    } else {
		fail("returned non zero");
	    }
	    return;
	}
	if (size1 != size3) {
            if (!small_port || size2 != size3) {
                MESSAGE("size1 = %d, size3 = %d\n",size1,size3);
                fail("decode and encode size differs");
                return;
            }
	}

	MESSAGE("ei_x_encode_%s, arg is type %s", t->name, t->type);
	err = t->ei_x_encode_fp(&arg, &objv[oix]);
	if (err != 0) {
	    if (err != -1) {
		fail("returned non zero but not -1");
	    } else {
		fail("returned non zero");
	    }
	    ei_x_free(&arg);
	    return;
	}
	if (arg.index < 1) {
	    fail("size is < 1");
	    ei_x_free(&arg);
	    return;
	}

	inp += size1;
	outp += size2;

	if (objv[oix].nterms) { /* container term */
	    if (++oix >= sizeof(objv)/sizeof(*objv))
		fail("Term too deep");
	}
	else { /* "leaf" term */
	    while (oix > 0) {
		if (--(objv[oix - 1].nterms) == 0) {
		    /* last element in container */
		    --oix;

		    size2 = 0;
		    err = ei_skip_term(objv[oix].startp, &size2);
		    if (err != 0) {
			fail("ei_skip_term returned non zero");
			return;
		    }
		    if (objv[oix].startp + size2 != inp) {
			MESSAGE("size1 = %d, size2 = %d\n", size1, size2);
			fail("container skip size differs");
			return;
		    }
		}
		else
		    break; /* more elements in container */
	    }
	}

    }
    if (oix > 0) {
	fail("Container not complete");
    }
    send_buffer(out_buf, outp - out_buf);
    send_buffer(arg.buff, arg.index);
    ei_x_free(&arg);
    free_packet(packet);
}

void decode_encode_one(struct Type* t)
{
    decode_encode(&t, 1);
}



void decode_encode_big(struct Type* t)
{
    char *buf;
    char buf2[2048];
    void *p; /* (TYPE*) */
    int size1 = 0;
    int size2 = 0;
    int size3 = 0;
    int err, index = 0, len, type;
    ei_x_buff arg;

    MESSAGE("ei_decode_%s, arg is type %s", t->name, t->type);
    buf = read_packet(NULL);
    ei_get_type(buf+1, &index, &type, &len);
    p = ei_alloc_big(len);
    err = t->ei_decode_fp(buf+1, &size1, p);
    if (err != 0) {
	if (err != -1) {
	    fail("decode returned non zero but not -1");
	} else {
	    fail("decode returned non zero");
	}
	return;
    }
    if (size1 < 1) {
	fail("size is < 1");
	return;
    }

    MESSAGE("ei_encode_%s buf is NULL, arg is type %s", t->name, t->type);
    err = t->ei_encode_fp(NULL, &size2, p);
    if (err != 0) {
	if (err != -1) {
	    fail("size calculation returned non zero but not -1");
	    return;
	} else {
	    fail("size calculation returned non zero");
	    return;
	}
    }
    if (size1 != size2) {
	MESSAGE("size1 = %d, size2 = %d\n",size1,size2);
	fail("decode and encode size differs when buf is NULL");
	return;
    }
    MESSAGE("ei_encode_%s, arg is type %s", t->name, t->type);
    err = t->ei_encode_fp(buf2, &size3, p);
    if (err != 0) {
	if (err != -1) {
	    fail("returned non zero but not -1");
	} else {
	    fail("returned non zero");
	}
	return;
    }
    if (size1 != size3) {
	MESSAGE("size1 = %d, size2 = %d\n",size1,size3);
	fail("decode and encode size differs");
	return;
    }
    send_buffer(buf2, size1);

    MESSAGE("ei_x_encode_%s, arg is type %s", t->name, t->type);
    ei_x_new(&arg);
    err = t->ei_x_encode_fp(&arg, p);
    if (err != 0) {
	if (err != -1) {
	    fail("returned non zero but not -1");
	} else {
	    fail("returned non zero");
	}
	ei_x_free(&arg);
	return;
    }
    if (arg.index < 1) {
	fail("size is < 1");
	ei_x_free(&arg);
	return;
    }
    send_buffer(arg.buff, arg.index);
    ei_x_free(&arg);
    ei_free_big(p);
    free_packet(buf);
}


void encode_bitstring(void)
{
    char* packet;
    char* inp;
    char out_buf[BUFSZ];
    int size;
    int err, i;
    ei_x_buff arg;
    const char* p;
    unsigned int bitoffs;
    size_t nbits, org_nbits;

    packet = read_packet(NULL);
    inp = packet+1;

    size = 0;
    err = ei_decode_bitstring(inp, &size, &p, &bitoffs, &nbits);
    if (err != 0) {
        fail1("ei_decode_bitstring returned non zero %d", err);
        return;
    }

    /*
     * Now send a bunch of different sub-bitstrings back
     * encoded both with ei_encode_ and ei_x_encode_.
     */
    org_nbits = nbits;
    do {
        size = 0;
        err = ei_encode_bitstring(out_buf, &size, p, bitoffs, nbits);
        if (err != 0) {
            fail1("ei_encode_bitstring returned non zero %d", err);
            return;
        }

        ei_x_new(&arg);
        err = ei_x_encode_bitstring(&arg, p, bitoffs, nbits);
        if (err != 0) {
            fail1("ei_x_encode_bitstring returned non zero %d", err);
            ei_x_free(&arg);
            return;
        }

        if (arg.index < 1) {
            fail("size is < 1");
            ei_x_free(&arg);
            return;
        }

        send_buffer(out_buf, size);
        send_buffer(arg.buff, arg.index);
        ei_x_free(&arg);

        bitoffs++;
        nbits -= (nbits / 20) + 1;
    } while (nbits < org_nbits);

    free_packet(packet);
}


/* ******************************************************************** */

TESTCASE(test_ei_decode_encode)
{
    int i;

    ei_init();

    decode_encode_one(&fun_type);
    decode_encode_one(&fun_type);
    decode_encode_one(&pid_type);
    decode_encode_one(&port_type);
    decode_encode_one(&ref_type);
    decode_encode_one(&trace_type);

    decode_encode_big(&big_type);
    decode_encode_big(&big_type);
    decode_encode_big(&big_type);

    decode_encode_big(&big_type);
    decode_encode_big(&big_type);
    decode_encode_big(&big_type);

    /* Test large node containers... */
    decode_encode_one(&pid_type);
    decode_encode_one(&port_type);
    decode_encode_one(&ref_type);

    for (i=0; i<5; i++) {
        decode_encode_one(&pid_type);
        decode_encode_one(&port_type);
        decode_encode_one(&ref_type);
        decode_encode_one(&ref_type);
    }

    /* Full 64-bit pids */
    for (i=16; i<=32; i++)
        decode_encode_one(&pid_type);

    /* Full 64-bit pids */
    for (i=24; i<=40; i++)
        decode_encode_one(&port_type);

    /* Unicode atoms */
    for (i=0; i<24; i++) {
	decode_encode_one(&my_atom_type);
	decode_encode_one(&pid_type);
	decode_encode_one(&port_type);
	decode_encode_one(&ref_type);
    }

    decode_encode_one(&tuple_type);  /* {} */
    {
	struct Type* tpl[] = { &tuple_type, &my_atom_type, &pid_type, &port_type, &ref_type };
	decode_encode(tpl, 5);
    }

    {
	struct Type* list[] = { &list_type, &my_atom_type, &pid_type, &port_type, &ref_type, &nil_type };
	decode_encode(list, 6);
    }
    {
	struct Type* list[] = { &list_type, &my_atom_type, &fun_type };
	decode_encode(list, 3);
    }
    decode_encode_one(&map_type);  /* #{} */
    { /* #{atom => atom}*/
	struct Type* map[] = { &map_type, &my_atom_type, &my_atom_type };
	decode_encode(map, 3);
    }

    { /* #{atom => atom, atom => pid, port => ref }*/
	struct Type* map[] = { &map_type,
	    &my_atom_type, &my_atom_type,
	    &my_atom_type, &pid_type,
	    &port_type, &ref_type
	};
	decode_encode(map, 7);
    }

    for (i=0; i <= 48; i++) {
        decode_encode_one(&my_bitstring_type);
    }

    encode_bitstring();

    report(1);
}

/* ******************************************************************** */
