/*

xstructmodule.c - C source file for Python module 'xstruct', which is
an extension of the standard Python 'struct' module

written by Robin Boerdijk (boerdijk@my-deja.com)
last modified: 1999-10-06

The first part of this file (up to the '===' box) is an integral copy 
of the orignal structmodule.c with the following modifications:

1. unpack_float() and unpack_double() have been modified to return
a double instead of a PyObject*. This way, the functions are more
general purpose and it makes them symmetrical to pack_float() and
pack_double(). bu_float(), bu_double(), lu_float() and lu_double()
have been changed to create the PyObject* from the returned double
now.

2. Added p_sstr(), p_pstr(), u_sstr() and u_pstr() for packing and
unpacking of 's' and 'p' type strings. These functions take the
string packing and unpacking code out of the struct_pack() and
struct_unpack() functions and make them available for general use.
Note that because of this, the error message of the exception thrown by 
p_sstr() and p_pstr() when the input object is not a PyStringObject 
differs from the one originally thrown by struct_pack(), but is now
consistent with the error messages from the np_*(), bp_*() and lp_*()
functions.

3. Modified calcsize() so that it optionally also counts the number of
objects specified by the format string.

4. Sligthly optimized struct_pack() and struct_unpack() by moving the 
packing and unpacking code for 'x', 's' and 'p' from the inner loop to 
the outer loop.

5. Optimized struct_unpack() to directly allocate and fill the result tuple 
(using the object count returned by calcsize()), instead of going through
a temporary list.

*/

/***********************************************************
Copyright 1991-1995 by Stichting Mathematisch Centrum, Amsterdam,
The Netherlands.

                        All Rights Reserved

Permission to use, copy, modify, and distribute this software and its
documentation for any purpose and without fee is hereby granted,
provided that the above copyright notice appear in all copies and that
both that copyright notice and this permission notice appear in
supporting documentation, and that the names of Stichting Mathematisch
Centrum or CWI or Corporation for National Research Initiatives or
CNRI not be used in advertising or publicity pertaining to
distribution of the software without specific, written prior
permission.

While CWI is the initial source for this software, a modified version
is made available by the Corporation for National Research Initiatives
(CNRI) at the Internet address ftp://ftp.python.org.

STICHTING MATHEMATISCH CENTRUM AND CNRI DISCLAIM ALL WARRANTIES WITH
REGARD TO THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS, IN NO EVENT SHALL STICHTING MATHEMATISCH
CENTRUM OR CNRI BE LIABLE FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL
DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
PERFORMANCE OF THIS SOFTWARE.

******************************************************************/

/* struct module -- pack values into and (out of) strings */

/* New version supporting byte order, alignment and size options,
   character strings, and unsigned numbers */

static char struct__doc__[] = "\
Functions to convert between Python values and C structs.\n\
Python strings are used to hold the data representing the C struct\n\
and also as format strings to describe the layout of data in the C struct.\n\
\n\
The optional first format char indicates byte ordering and alignment:\n\
 @: native w/native alignment(default)\n\
 =: native w/standard alignment\n\
 <: little-endian, std. alignment\n\
 >: big-endian, std. alignment\n\
 !: network, std (same as >)\n\
\n\
The remaining chars indicate types of args and must match exactly;\n\
these can be preceded by a decimal repeat count:\n\
 x: pad byte (no data); c:char; b:signed byte; B:unsigned byte;\n\
 h:short; H:unsigned short; i:int; I:unsigned int;\n\
 l:long; L:unsigned long; f:float; d:double.\n\
Special cases (preceding decimal count indicates length):\n\
 s:string (array of char); p: pascal string (w. count byte).\n\
Special case (only available in native format):\n\
 P:an integer type that is wide enough to hold a pointer.\n\
Whitespace between formats is ignored.\n\
\n\
The variable struct.error is an exception raised on errors.";

#include "Python.h"
#include "mymath.h"

#include <limits.h>
#include <ctype.h>


/* Exception */

static PyObject *StructError;


/* Define various structs to figure out the alignments of types */

#ifdef __MWERKS__
/*
** XXXX We have a problem here. There are no unique alignment rules
** on the PowerPC mac. 
*/
#ifdef __powerc
#pragma options align=mac68k
#endif
#endif /* __MWERKS__ */

typedef struct { char c; short x; } s_short;
typedef struct { char c; int x; } s_int;
typedef struct { char c; long x; } s_long;
typedef struct { char c; float x; } s_float;
typedef struct { char c; double x; } s_double;
typedef struct { char c; void *x; } s_void_p;

#define SHORT_ALIGN (sizeof(s_short) - sizeof(short))
#define INT_ALIGN (sizeof(s_int) - sizeof(int))
#define LONG_ALIGN (sizeof(s_long) - sizeof(long))
#define FLOAT_ALIGN (sizeof(s_float) - sizeof(float))
#define DOUBLE_ALIGN (sizeof(s_double) - sizeof(double))
#define VOID_P_ALIGN (sizeof(s_void_p) - sizeof(void *))

#ifdef __powerc
#pragma options align=reset
#endif

/* Helper routine to get a Python integer and raise the appropriate error
   if it isn't one */

static int
get_long(v, p)
	PyObject *v;
	long *p;
{
	long x = PyInt_AsLong(v);
	if (x == -1 && PyErr_Occurred()) {
		if (PyErr_ExceptionMatches(PyExc_TypeError))
			PyErr_SetString(StructError,
					"required argument is not an integer");
		return -1;
	}
	*p = x;
	return 0;
}


/* Same, but handling unsigned long */

static int
get_ulong(v, p)
	PyObject *v;
	unsigned long *p;
{
	if (PyLong_Check(v)) {
		unsigned long x = PyLong_AsUnsignedLong(v);
		if (x == (unsigned long)(-1) && PyErr_Occurred())
			return -1;
		*p = x;
		return 0;
	}
	else {
		return get_long(v, (long *)p);
	}
}


/* Floating point helpers */

/* These use ANSI/IEEE Standard 754-1985 (Standard for Binary Floating
   Point Arithmetic).  See the following URL:
   http://www.psc.edu/general/software/packages/ieee/ieee.html */

/* XXX Inf/NaN are not handled quite right (but underflow is!) */

static int
pack_float(x, p, incr)
	double x; /* The number to pack */
	char *p;  /* Where to pack the high order byte */
	int incr; /* 1 for big-endian; -1 for little-endian */
{
	int s;
	int e;
	double f;
	long fbits;

	if (x < 0) {
		s = 1;
		x = -x;
	}
	else
		s = 0;

	f = frexp(x, &e);

	/* Normalize f to be in the range [1.0, 2.0) */
	if (0.5 <= f && f < 1.0) {
		f *= 2.0;
		e--;
	}
	else if (f == 0.0) {
		e = 0;
	}
	else {
		PyErr_SetString(PyExc_SystemError,
				"frexp() result out of range");
		return -1;
	}

	if (e >= 128) {
		/* XXX 128 itself is reserved for Inf/NaN */
		PyErr_SetString(PyExc_OverflowError,
				"float too large to pack with f format");
		return -1;
	}
	else if (e < -126) {
		/* Gradual underflow */
		f = ldexp(f, 126 + e);
		e = 0;
	}
	else if (!(e == 0 && f == 0.0)) {
		e += 127;
		f -= 1.0; /* Get rid of leading 1 */
	}

	f *= 8388608.0; /* 2**23 */
	fbits = (long) floor(f + 0.5); /* Round */

	/* First byte */
	*p = (s<<7) | (e>>1);
	p += incr;

	/* Second byte */
	*p = (char) (((e&1)<<7) | (fbits>>16));
	p += incr;

	/* Third byte */
	*p = (fbits>>8) & 0xFF;
	p += incr;

	/* Fourth byte */
	*p = fbits&0xFF;

	/* Done */
	return 0;
}

static int
pack_double(x, p, incr)
	double x; /* The number to pack */
	char *p;  /* Where to pack the high order byte */
	int incr; /* 1 for big-endian; -1 for little-endian */
{
	int s;
	int e;
	double f;
	long fhi, flo;

	if (x < 0) {
		s = 1;
		x = -x;
	}
	else
		s = 0;

	f = frexp(x, &e);

	/* Normalize f to be in the range [1.0, 2.0) */
	if (0.5 <= f && f < 1.0) {
		f *= 2.0;
		e--;
	}
	else if (f == 0.0) {
		e = 0;
	}
	else {
		PyErr_SetString(PyExc_SystemError,
				"frexp() result out of range");
		return -1;
	}

	if (e >= 1024) {
		/* XXX 1024 itself is reserved for Inf/NaN */
		PyErr_SetString(PyExc_OverflowError,
				"float too large to pack with d format");
		return -1;
	}
	else if (e < -1022) {
		/* Gradual underflow */
		f = ldexp(f, 1022 + e);
		e = 0;
	}
	else if (!(e == 0 && f == 0.0)) {
		e += 1023;
		f -= 1.0; /* Get rid of leading 1 */
	}

	/* fhi receives the high 28 bits; flo the low 24 bits (== 52 bits) */
	f *= 268435456.0; /* 2**28 */
	fhi = (long) floor(f); /* Truncate */
	f -= (double)fhi;
	f *= 16777216.0; /* 2**24 */
	flo = (long) floor(f + 0.5); /* Round */

	/* First byte */
	*p = (s<<7) | (e>>4);
	p += incr;

	/* Second byte */
	*p = (char) (((e&0xF)<<4) | (fhi>>24));
	p += incr;

	/* Third byte */
	*p = (fhi>>16) & 0xFF;
	p += incr;

	/* Fourth byte */
	*p = (fhi>>8) & 0xFF;
	p += incr;

	/* Fifth byte */
	*p = fhi & 0xFF;
	p += incr;

	/* Sixth byte */
	*p = (flo>>16) & 0xFF;
	p += incr;

	/* Seventh byte */
	*p = (flo>>8) & 0xFF;
	p += incr;

	/* Eighth byte */
	*p = flo & 0xFF;
	p += incr;

	/* Done */
	return 0;
}

static double
unpack_float(p, incr)
	char *p;  /* Where the high order byte is */
	int incr; /* 1 for big-endian; -1 for little-endian */
{
	int s;
	int e;
	long f;
	double x;

	/* First byte */
	s = (*p>>7) & 1;
	e = (*p & 0x7F) << 1;
	p += incr;

	/* Second byte */
	e |= (*p>>7) & 1;
	f = (*p & 0x7F) << 16;
	p += incr;

	/* Third byte */
	f |= (*p & 0xFF) << 8;
	p += incr;

	/* Fourth byte */
	f |= *p & 0xFF;

	x = (double)f / 8388608.0;

	/* XXX This sadly ignores Inf/NaN issues */
	if (e == 0)
		e = -126;
	else {
		x += 1.0;
		e -= 127;
	}
	x = ldexp(x, e);

	if (s)
		x = -x;

  return x;
}

static double
unpack_double(p, incr)
	char *p;  /* Where the high order byte is */
	int incr; /* 1 for big-endian; -1 for little-endian */
{
	int s;
	int e;
	long fhi, flo;
	double x;

	/* First byte */
	s = (*p>>7) & 1;
	e = (*p & 0x7F) << 4;
	p += incr;

	/* Second byte */
	e |= (*p>>4) & 0xF;
	fhi = (*p & 0xF) << 24;
	p += incr;

	/* Third byte */
	fhi |= (*p & 0xFF) << 16;
	p += incr;

	/* Fourth byte */
	fhi |= (*p & 0xFF) << 8;
	p += incr;

	/* Fifth byte */
	fhi |= *p & 0xFF;
	p += incr;

	/* Sixth byte */
	flo = (*p & 0xFF) << 16;
	p += incr;

	/* Seventh byte */
	flo |= (*p & 0xFF) << 8;
	p += incr;

	/* Eighth byte */
	flo |= *p & 0xFF;
	p += incr;

	x = (double)fhi + (double)flo / 16777216.0; /* 2**24 */
	x /= 268435456.0; /* 2**28 */

	/* XXX This sadly ignores Inf/NaN */
	if (e == 0)
		e = -1022;
	else {
		x += 1.0;
		e -= 1023;
	}
	x = ldexp(x, e);

	if (s)
		x = -x;

  return x;
}


/* The translation function for each format character is table driven */

typedef struct _formatdef {
	char format;
	int size;
	int alignment;
	PyObject* (*unpack) Py_PROTO((const char *,
				      const struct _formatdef *));
	int (*pack) Py_PROTO((char *,
			      PyObject *,
			      const struct _formatdef *));
} formatdef;

static PyObject *
nu_char(p, f)
	const char *p;
	const formatdef *f;
{
	return PyString_FromStringAndSize(p, 1);
}

static PyObject *
nu_byte(p, f)
	const char *p;
	const formatdef *f;
{
	return PyInt_FromLong((long) *(signed char *)p);
}

static PyObject *
nu_ubyte(p, f)
	const char *p;
	const formatdef *f;
{
	return PyInt_FromLong((long) *(unsigned char *)p);
}

static PyObject *
nu_short(p, f)
	const char *p;
	const formatdef *f;
{
	return PyInt_FromLong((long) *(short *)p);
}

static PyObject *
nu_ushort(p, f)
	const char *p;
	const formatdef *f;
{
	return PyInt_FromLong((long) *(unsigned short *)p);
}

static PyObject *
nu_int(p, f)
	const char *p;
	const formatdef *f;
{
	return PyInt_FromLong((long) *(int *)p);
}

static PyObject *
nu_uint(p, f)
	const char *p;
	const formatdef *f;
{
	unsigned int x = *(unsigned int *)p;
	return PyLong_FromUnsignedLong((unsigned long)x);
}

static PyObject *
nu_long(p, f)
	const char *p;
	const formatdef *f;
{
	return PyInt_FromLong(*(long *)p);
}

static PyObject *
nu_ulong(p, f)
	const char *p;
	const formatdef *f;
{
	return PyLong_FromUnsignedLong(*(unsigned long *)p);
}

static PyObject *
nu_float(p, f)
	const char *p;
	const formatdef *f;
{
	float x;
	memcpy((char *)&x, p, sizeof(float));
	return PyFloat_FromDouble((double)x);
}

static PyObject *
nu_double(p, f)
	const char *p;
	const formatdef *f;
{
	double x;
	memcpy((char *)&x, p, sizeof(double));
	return PyFloat_FromDouble(x);
}

static PyObject *
nu_void_p(p, f)
	const char *p;
	const formatdef *f;
{
	return PyLong_FromVoidPtr(*(void **)p);
}

static int
np_byte(p, v, f)
	char *p;
	PyObject *v;
	const formatdef *f;
{
	long x;
	if (get_long(v, &x) < 0)
		return -1;
	*p = (char)x;
	return 0;
}

static int
np_char(p, v, f)
	char *p;
	PyObject *v;
	const formatdef *f;
{
	if (!PyString_Check(v) || PyString_Size(v) != 1) {
		PyErr_SetString(StructError,
				"char format require string of length 1");
		return -1;
	}
	*p = *PyString_AsString(v);
	return 0;
}

static int
np_short(p, v, f)
	char *p;
	PyObject *v;
	const formatdef *f;
{
	long x;
	if (get_long(v, &x) < 0)
		return -1;
	* (short *)p = (short)x;
	return 0;
}

static int
np_int(p, v, f)
	char *p;
	PyObject *v;
	const formatdef *f;
{
	long x;
	if (get_long(v, &x) < 0)
		return -1;
	* (int *)p = x;
	return 0;
}

static int
np_uint(p, v, f)
	char *p;
	PyObject *v;
	const formatdef *f;
{
	unsigned long x;
	if (get_ulong(v, &x) < 0)
		return -1;
	* (unsigned int *)p = x;
	return 0;
}

static int
np_long(p, v, f)
	char *p;
	PyObject *v;
	const formatdef *f;
{
	long x;
	if (get_long(v, &x) < 0)
		return -1;
	* (long *)p = x;
	return 0;
}

static int
np_ulong(p, v, f)
	char *p;
	PyObject *v;
	const formatdef *f;
{
	unsigned long x;
	if (get_ulong(v, &x) < 0)
		return -1;
	* (unsigned long *)p = x;
	return 0;
}

static int
np_float(p, v, f)
	char *p;
	PyObject *v;
	const formatdef *f;
{
	float x = (float)PyFloat_AsDouble(v);
	if (x == -1 && PyErr_Occurred()) {
		PyErr_SetString(StructError,
				"required argument is not a float");
		return -1;
	}
	memcpy(p, (char *)&x, sizeof(float));
	return 0;
}

static int
np_double(p, v, f)
	char *p;
	PyObject *v;
	const formatdef *f;
{
	double x = PyFloat_AsDouble(v);
	if (x == -1 && PyErr_Occurred()) {
		PyErr_SetString(StructError,
				"required argument is not a float");
		return -1;
	}
	memcpy(p, (char *)&x, sizeof(double));
	return 0;
}

static int
np_void_p(p, v, f)
	char *p;
	PyObject *v;
	const formatdef *f;
{
	void *x = PyLong_AsVoidPtr(v);
	if (x == NULL && PyErr_Occurred()) {
		/* ### hrm. PyLong_AsVoidPtr raises SystemError */
		if (PyErr_ExceptionMatches(PyExc_TypeError))
			PyErr_SetString(StructError,
					"required argument is not an integer");
		return -1;
	}
	*(void **)p = x;
	return 0;
}

static formatdef native_table[] = {
	{'x',	sizeof(char),	0,		NULL},
	{'b',	sizeof(char),	0,		nu_byte,	np_byte},
	{'B',	sizeof(char),	0,		nu_ubyte,	np_byte},
	{'c',	sizeof(char),	0,		nu_char,	np_char},
	{'s',	sizeof(char),	0,		NULL},
	{'p',	sizeof(char),	0,		NULL},
	{'h',	sizeof(short),	SHORT_ALIGN,	nu_short,	np_short},
	{'H',	sizeof(short),	SHORT_ALIGN,	nu_ushort,	np_short},
	{'i',	sizeof(int),	INT_ALIGN,	nu_int,		np_int},
	{'I',	sizeof(int),	INT_ALIGN,	nu_uint,	np_uint},
	{'l',	sizeof(long),	LONG_ALIGN,	nu_long,	np_long},
	{'L',	sizeof(long),	LONG_ALIGN,	nu_ulong,	np_ulong},
	{'f',	sizeof(float),	FLOAT_ALIGN,	nu_float,	np_float},
	{'d',	sizeof(double),	DOUBLE_ALIGN,	nu_double,	np_double},
	{'P',	sizeof(void *),	VOID_P_ALIGN,	nu_void_p,	np_void_p},
	{0}
};

static PyObject *
bu_int(p, f)
	const char *p;
	const formatdef *f;
{
	long x = 0;
	int i = f->size;
	do {
		x = (x<<8) | (*p++ & 0xFF);
	} while (--i > 0);
	i = 8*(sizeof(long) - f->size);
	if (i) {
		x <<= i;
		x >>= i;
	}
	return PyInt_FromLong(x);
}

static PyObject *
bu_uint(p, f)
	const char *p;
	const formatdef *f;
{
	unsigned long x = 0;
	int i = f->size;
	do {
		x = (x<<8) | (*p++ & 0xFF);
	} while (--i > 0);
	if (f->size >= 4)
		return PyLong_FromUnsignedLong(x);
	else
		return PyInt_FromLong((long)x);
}

static PyObject *
bu_float(p, f)
	const char *p;
	const formatdef *f;
{
	return PyFloat_FromDouble(unpack_float(p, 1));
}

static PyObject *
bu_double(p, f)
	const char *p;
	const formatdef *f;
{
	return PyFloat_FromDouble(unpack_double(p, 1));
}

static int
bp_int(p, v, f)
	char *p;
	PyObject *v;
	const formatdef *f;
{
	long x;
	int i;
	if (get_long(v, &x) < 0)
		return -1;
	i = f->size;
	do {
		p[--i] = (char)x;
		x >>= 8;
	} while (i > 0);
	return 0;
}

static int
bp_uint(p, v, f)
	char *p;
	PyObject *v;
	const formatdef *f;
{
	unsigned long x;
	int i;
	if (get_ulong(v, &x) < 0)
		return -1;
	i = f->size;
	do {
		p[--i] = (char)x;
		x >>= 8;
	} while (i > 0);
	return 0;
}

static int
bp_float(p, v, f)
	char *p;
	PyObject *v;
	const formatdef *f;
{
	double x = PyFloat_AsDouble(v);
	if (x == -1 && PyErr_Occurred()) {
		PyErr_SetString(StructError,
				"required argument is not a float");
		return -1;
	}
	return pack_float(x, p, 1);
}

static int
bp_double(p, v, f)
	char *p;
	PyObject *v;
	const formatdef *f;
{
	double x = PyFloat_AsDouble(v);
	if (x == -1 && PyErr_Occurred()) {
		PyErr_SetString(StructError,
				"required argument is not a float");
		return -1;
	}
	return pack_double(x, p, 1);
}

static formatdef bigendian_table[] = {
	{'x',	1,		0,		NULL},
	{'b',	1,		0,		bu_int,		bp_int},
	{'B',	1,		0,		bu_uint,	bp_int},
	{'c',	1,		0,		nu_char,	np_char},
	{'s',	1,		0,		NULL},
	{'p',	1,		0,		NULL},
	{'h',	2,		0,		bu_int,		bp_int},
	{'H',	2,		0,		bu_uint,	bp_uint},
	{'i',	4,		0,		bu_int,		bp_int},
	{'I',	4,		0,		bu_uint,	bp_uint},
	{'l',	4,		0,		bu_int,		bp_int},
	{'L',	4,		0,		bu_uint,	bp_uint},
	{'f',	4,		0,		bu_float,	bp_float},
	{'d',	8,		0,		bu_double,	bp_double},
	{0}
};

static PyObject *
lu_int(p, f)
	const char *p;
	const formatdef *f;
{
	long x = 0;
	int i = f->size;
	do {
		x = (x<<8) | (p[--i] & 0xFF);
	} while (i > 0);
	i = 8*(sizeof(long) - f->size);
	if (i) {
		x <<= i;
		x >>= i;
	}
	return PyInt_FromLong(x);
}

static PyObject *
lu_uint(p, f)
	const char *p;
	const formatdef *f;
{
	unsigned long x = 0;
	int i = f->size;
	do {
		x = (x<<8) | (p[--i] & 0xFF);
	} while (i > 0);
	if (f->size >= 4)
		return PyLong_FromUnsignedLong(x);
	else
		return PyInt_FromLong((long)x);
}

static PyObject *
lu_float(p, f)
	const char *p;
	const formatdef *f;
{
	return PyFloat_FromDouble(unpack_float(p+3, -1));
}

static PyObject *
lu_double(p, f)
	const char *p;
	const formatdef *f;
{
	return PyFloat_FromDouble(unpack_double(p+7, -1));
}

static int
lp_int(p, v, f)
	char *p;
	PyObject *v;
	const formatdef *f;
{
	long x;
	int i;
	if (get_long(v, &x) < 0)
		return -1;
	i = f->size;
	do {
		*p++ = (char)x;
		x >>= 8;
	} while (--i > 0);
	return 0;
}

static int
lp_uint(p, v, f)
	char *p;
	PyObject *v;
	const formatdef *f;
{
	unsigned long x;
	int i;
	if (get_ulong(v, &x) < 0)
		return -1;
	i = f->size;
	do {
		*p++ = (char)x;
		x >>= 8;
	} while (--i > 0);
	return 0;
}

static int
lp_float(p, v, f)
	char *p;
	PyObject *v;
	const formatdef *f;
{
	double x = PyFloat_AsDouble(v);
	if (x == -1 && PyErr_Occurred()) {
		PyErr_SetString(StructError,
				"required argument is not a float");
		return -1;
	}
	return pack_float(x, p+3, -1);
}

static int
lp_double(p, v, f)
	char *p;
	PyObject *v;
	const formatdef *f;
{
	double x = PyFloat_AsDouble(v);
	if (x == -1 && PyErr_Occurred()) {
		PyErr_SetString(StructError,
				"required argument is not a float");
		return -1;
	}
	return pack_double(x, p+7, -1);
}

static formatdef lilendian_table[] = {
	{'x',	1,		0,		NULL},
	{'b',	1,		0,		lu_int,		lp_int},
	{'B',	1,		0,		lu_uint,	lp_int},
	{'c',	1,		0,		nu_char,	np_char},
	{'s',	1,		0,		NULL},
	{'p',	1,		0,		NULL},
	{'h',	2,		0,		lu_int,		lp_int},
	{'H',	2,		0,		lu_uint,	lp_uint},
	{'i',	4,		0,		lu_int,		lp_int},
	{'I',	4,		0,		lu_uint,	lp_uint},
	{'l',	4,		0,		lu_int,		lp_int},
	{'L',	4,		0,		lu_uint,	lp_uint},
	{'f',	4,		0,		lu_float,	lp_float},
	{'d',	8,		0,		lu_double,	lp_double},
	{0}
};

static const formatdef *
whichtable(pfmt)
	const char **pfmt;
{
	const char *fmt = (*pfmt)++; /* May be backed out of later */
	switch (*fmt) {
	case '<':
		return lilendian_table;
	case '>':
	case '!': /* Network byte order is big-endian */
		return bigendian_table;
	case '=': { /* Host byte order -- different from native in aligment! */
		int n = 1;
		char *p = (char *) &n;
		if (*p == 1)
			return lilendian_table;
		else
			return bigendian_table;
	}
	default:
		--*pfmt; /* Back out of pointer increment */
		/* Fall through */
	case '@':
		return native_table;
	}
}

static int p_sstr(char* p, PyObject* v, int num)
{
  if (!PyString_Check(v)) 
  {
    PyErr_SetString(StructError, "required argument is not a string");
    return -1;
  }
  else
  {
    int	n = PyString_Size(v);
		
    if (n > num)
		  n = num;
		
    if (n > 0)
		  memcpy(p, PyString_AsString(v), n);
		
    if (n < num)
		  memset(p + n, '\0', num - n);
    
    return 0;
  }
}

static int p_pstr(char* p, PyObject* v, int num)
{
  if (!PyString_Check(v)) 
  {
    PyErr_SetString(StructError, "required argument is not a string");
    return -1;
  }
  else
  {
	  int n = PyString_Size(v);
    
    num--; /* now num is max string size */

	  if (n > num)
      n = num;

    *p++ = n; /* store the length byte */

	  if (n > 0)
	    memcpy(p, PyString_AsString(v), n);
		
    if (n < num)
	    memset(p + n, '\0', num - n);
    
    return 0;
  }
}

static PyObject* u_sstr(char* p, int num)
{
  return PyString_FromStringAndSize(p, num);
}

static PyObject* u_pstr(char* p, int num)
{
  int n = *(unsigned char*) p; /* first byte is string size */
  if (n >= num)
    n = num-1;
  return PyString_FromStringAndSize(p + 1, n);
}

/* Get the table entry for a format code */

static const formatdef *
getentry(c, f)
	int c;
	const formatdef *f;
{
	for (; f->format != '\0'; f++) {
		if (f->format == c) {
			return f;
		}
	}
	PyErr_SetString(StructError, "bad char in struct format");
	return NULL;
}


/* Align a size according to a format code */

static int
align(size, c, e)
	int size;
	int c;
	const formatdef *e;
{
	if (e->format == c) {
		if (e->alignment) {
			size = ((size + e->alignment - 1)
				/ e->alignment)
				* e->alignment;
		}
	}
	return size;
}

/* calculate the size of a format string */

static int
calcsize(fmt, f, objc)
	const char *fmt;
	const formatdef *f;
  int* objc;
{
	const formatdef *e;
	const char *s;
	char c;
	int size,  num, itemsize, x;

	s = fmt;
	size = 0;

  if (objc != NULL)
    *objc = 0;

	while ((c = *s++) != '\0') {
		if (isspace((int)c))
			continue;
		if ('0' <= c && c <= '9') {
			num = c - '0';
			while ('0' <= (c = *s++) && c <= '9') {
				x = num*10 + (c - '0');
				if (x/10 != num) {
					PyErr_SetString(
						StructError,
						"overflow in item count");
					return -1;
				}
				num = x;
			}
			if (c == '\0')
				break;
		}
		else
			num = 1;
		
		e = getentry(c, f);
		if (e == NULL)
			return -1;

		itemsize = e->size;
		size = align(size, c, e);
		x = num * itemsize;
		size += x;
		if (x/itemsize != num || size < 0) {
			PyErr_SetString(StructError, "total struct size too long");
			return -1;
		}

    if (objc != NULL)
    {
      switch (c)
      {
        case 's':
        {
          *objc += 1; 
          break;
        }
        case 'p':
        {
          if (num != 0)
            *objc += 1; 
          break;
        }
        case 'x':
        {
          break;
        }
        default:
        {
          *objc += num;
        }
      }
    }
	}

	return size;
}


static char calcsize__doc__[] = "\
calcsize(fmt) -> int\n\
Return size of C struct described by format string fmt.\n\
See struct.__doc__ for more on format strings.";

static PyObject *
struct_calcsize(self, args)
	PyObject *self; /* Not used */
	PyObject *args;
{
	char *fmt;
	const formatdef *f;
	int size;

	if (!PyArg_ParseTuple(args, "s", &fmt))
		return NULL;
	f = whichtable(&fmt);
	size = calcsize(fmt, f, NULL);
	if (size < 0)
		return NULL;
	return PyInt_FromLong((long)size);
}


static char pack__doc__[] = "\
pack(fmt, v1, v2, ...) -> string\n\
Return string containing values v1, v2, ... packed according to fmt.\n\
See struct.__doc__ for more on format strings.";

static PyObject *
struct_pack(self, args)
	PyObject *self; /* Not used */
	PyObject *args;
{
	const formatdef *f, *e;
	PyObject *format, *result, *v;
	char *fmt;
	int size, num;
	int i, n;
	char *s, *res, *restart, *nres;
	char c;

	if (args == NULL || !PyTuple_Check(args) ||
	    (n = PyTuple_Size(args)) < 1)
        {
		PyErr_BadArgument();
		return NULL;
	}
	format = PyTuple_GetItem(args, 0);
	if (!PyArg_Parse(format, "s", &fmt))
		return NULL;
	f = whichtable(&fmt);
	size = calcsize(fmt, f, NULL);
	if (size < 0)
		return NULL;
	result = PyString_FromStringAndSize((char *)NULL, size);
	if (result == NULL)
		return NULL;

	s = fmt;
	i = 1;
	res = restart = PyString_AsString(result);

	while ((c = *s++) != '\0') {
		if (isspace((int)c))
			continue;
		if ('0' <= c && c <= '9') {
			num = c - '0';
			while ('0' <= (c = *s++) && c <= '9')
			       num = num*10 + (c - '0');
			if (c == '\0')
				break;
		}
		else
			num = 1;

		e = getentry(c, f);
		if (e == NULL)
			goto fail;
		nres = restart + align((int)(res-restart), c, e);
		/* Fill padd bytes with zeros */
		while (res < nres)
			*res++ = '\0';

		if (num == 0 && c != 's') /* only '0s' consumes an argument */
			continue;

		if (c == 'x') /* doesn't consume arguments */
    {
			memset(res, '\0', num);
			res += num;
      continue;
    }
    
    if (i >= n) 
    {
			PyErr_SetString(StructError, "insufficient arguments to pack");
			goto fail;
		}

    if (c == 's' || c == 'p') /* num is string size, not repeat count */
    { 
		  v = PyTuple_GetItem(args, i++);
		  if (v == NULL)
			  goto fail;

		  if (c == 's') 
      {
        if (p_sstr(res, v, num) != 0)
          goto fail;
      }
      else
      {
        if (p_pstr(res, v, num) != 0)
          goto fail;
      }

		  res += num;
		  continue;
	  }

		while (num > 0)
    {	
    	v = PyTuple_GetItem(args, i++);
      if (v == NULL)
        goto fail;
			if (e->pack(res, v, e) < 0)
				goto fail;
			res += e->size;
      num--;
    }
	}

	if (i < n) {
		PyErr_SetString(StructError,
				"too many arguments for pack format");
		goto fail;
	}

	return result;

 fail:
	Py_DECREF(result);
	return NULL;
}


static char unpack__doc__[] = "\
unpack(fmt, string) -> (v1, v2, ...)\n\
Unpack the string, containing packed C structure data, according\n\
to fmt.  Requires len(string)==calcsize(fmt).\n\
See struct.__doc__ for more on format strings.";

static PyObject *
struct_unpack(self, args)
	PyObject *self; /* Not used */
	PyObject *args;
{
	const formatdef *f, *e;
	char *str, *start, *fmt, *s;
	char c;
	int len, size, num, i, objc;
	PyObject *res, *v;

	if (!PyArg_ParseTuple(args, "ss#", &fmt, &start, &len))
		return NULL;
	f = whichtable(&fmt);
	size = calcsize(fmt, f, &objc);
	if (size < 0)
		return NULL;
	if (size != len) {
		PyErr_SetString(StructError,
				"unpack str size does not match format");
		return NULL;
	}

  res = PyTuple_New(objc);
  if (res == NULL)
    return NULL;

  i = 0;

	str = start;
	s = fmt;
	while ((c = *s++) != '\0') {
		if (isspace((int)c))
			continue;
		if ('0' <= c && c <= '9') {
			num = c - '0';
			while ('0' <= (c = *s++) && c <= '9')
			       num = num*10 + (c - '0');
			if (c == '\0')
				break;
		}
		else
			num = 1;

		e = getentry(c, f);
		if (e == NULL)
			goto fail;
		str = start + align((int)(str-start), c, e);

		if (num == 0 && c != 's') /* only '0s' creates an object */
			continue;

		if (c == 'x') /* doesn't create an object */
    {
			str += num;
      continue;
    }

		if (c == 's' || c == 'p') /* num is string size, not repeat count */
    {
      if (c == 's')
        v = u_sstr(str, num);
      else
	      v = u_pstr(str, num);

	    if (v == NULL)
        goto fail;

      PyTuple_SET_ITEM(res, i++, v); /* steals the reference !! */

      str += num;
      continue;
    }

    while (num > 0)
    {
			v = e->unpack(str, e);
			if (v == NULL)
				goto fail;
      PyTuple_SET_ITEM(res, i++, v); /* steals the reference !! */
			str += e->size;
      num--;
		}
	}

	return res;

 fail:
	Py_DECREF(res);
	return NULL;
}

/*===========*/
/* new stuff */
/*===========*/

#define FLAG_READONLY 1

/*---------------*/
/* PyStructField */
/*---------------*/

typedef struct {
  PyObject_HEAD
  PyObject* Name;
  const formatdef* Format;
  int Changeable;
  int RepeatCount;
  int Offset;
} PyStructField;

static void PyStructField_dealloc(PyStructField* self)
{
  if (self->Name != NULL)
    Py_DECREF(self->Name);

  PyMem_DEL(self);
}

PyTypeObject PyStructField_Type = {
	PyObject_HEAD_INIT(0) /* set in initxstruct() */
	0,
	"structfield",
	sizeof(PyStructField),
	0,
	(destructor)PyStructField_dealloc, /*tp_dealloc*/
	0,		/*tp_print*/
	0,		/*tp_getattr*/
	0,		/*tp_setattr*/
	0,		/*tp_compare*/
	0,		/*tp_repr*/
	0,		/*tp_as_number*/
	0,		/*tp_as_sequence*/
	0,		/*tp_as_mapping*/
	0,		/*tp_hash*/
	0,		/*tp_call*/
	0,		/*tp_str*/
	0,		/*tp_getattro*/
	0,		/*tp_setattro*/
	0,		/*tp_as_buffer*/
	Py_TPFLAGS_DEFAULT,	/*tp_flags*/
	0,		/*tp_doc*/
};

PyStructField* NewPyStructField()
{
  PyStructField* StructField = 
    PyObject_NEW(PyStructField, &PyStructField_Type);
  if (StructField == NULL)
    return NULL;

  StructField->Name = NULL;

  return StructField;
}
 
static PyObject* GetFieldValue(PyStructField* Field, char* StructData)
{
  char* FieldData = StructData + Field->Offset;

  switch (Field->Format->format)
  {
    case 's': 
    {
      return u_sstr(FieldData, Field->RepeatCount);
    }
    case 'p':
	  {
	    return u_pstr(FieldData, Field->RepeatCount);
	  }
	  default:
	  {
	    if (Field->RepeatCount == 1)
	    {
	      return Field->Format->unpack(FieldData, Field->Format);
	    }
	    else
	    {
	      PyObject* ResultTuple;
		    int i;
		
		    ResultTuple = PyTuple_New(Field->RepeatCount);
	      if (ResultTuple == NULL)
		      return NULL;
	    
	      i = 0;
	  
	      while (i < Field->RepeatCount)
		    {
		      PyObject* Element = Field->Format->unpack(FieldData, 
            Field->Format);
		      if (Element == NULL)
		      {
		        Py_DECREF(ResultTuple);
			      return NULL;
		      };

		      PyTuple_SET_ITEM(ResultTuple, i, Element); 
            /* steals the reference */
		  
		      FieldData += Field->Format->size;

		      i++;
		    }

		    return ResultTuple;
	    }
	  }
  }
}

static int SetFieldValue(PyStructField* Field, char* StructData, 
  PyObject* Value)
{
  char* FieldData = StructData + Field->Offset;

  switch (Field->Format->format)
  {
	  case 's': 
	  {
	    return p_sstr(FieldData, Value, Field->RepeatCount);
	  }
	  case 'p': 
	  {
	    return p_pstr(FieldData, Value, Field->RepeatCount);
	  }
	  default:
	  {
	    if (Field->RepeatCount == 1)
	    {
	      return Field->Format->pack(FieldData, Value, Field->Format);
	    }
	    else
	    {
	      int i;

	      if (!PyTuple_Check(Value)) 
	      {
		      PyErr_SetString(StructError, "value for field must be a tuple");
		      return -1;
		    }

		    if (PyTuple_Size(Value) != Field->RepeatCount)
		    {
		      PyErr_SetString(StructError, "field element count mismatch");
		      return -1;
		    }

		    i = 0;

		    while (i < Field->RepeatCount)
		    {
		      PyObject* Element = PyTuple_GET_ITEM(Value, i);
		        /* borrowed reference ! */

		      if (Field->Format->pack(FieldData, Element, Field->Format) < 0)
		        return -1;
		  
		      FieldData += Field->Format->size;

		      i++;
		    }

		    return 0;
	    }
	  }
  }
}

/*--------------------*/
/* PyStructDefinition */
/*--------------------*/

typedef struct {
  PyObject_HEAD
  const formatdef* FormatTable;
  PyObject* FieldList;
  PyObject* FieldMap;
  int StructSize;
  char* InitialStructData;
} PyStructDefinition;

static void PyStructDefinition_dealloc(PyStructDefinition* self)
{
  if (self->InitialStructData != NULL)
    free(self->InitialStructData);

  if (self->FieldMap != NULL)
    Py_DECREF(self->FieldMap);
    
  if (self->FieldList != NULL)
    Py_DECREF(self->FieldList);

  PyMem_DEL(self);
}

static PyObject* PyStructDefinition_getattr(PyStructDefinition* self, 
  char* name)
{
  if (strcmp(name, "size") == 0)
    return PyInt_FromLong(self->StructSize);

  PyErr_SetString(PyExc_AttributeError, name);
  return NULL;
}

/* forward declaration */

static PyObject* NewStructObject(PyStructDefinition* StructDefinition, 
  char* data, int len);

static PyObject* PyStructDefinition_call(PyStructDefinition* self, 
  PyObject* args, PyObject* kwds)
{
  char* data = NULL;
  int len;
 
  if (!PyArg_ParseTuple(args, "|s#", &data, &len))
    return NULL;

  if (data == NULL)
	return NewStructObject(self, self->InitialStructData, 
      self->StructSize);
  else
    return NewStructObject(self, data, len);
}

PyTypeObject PyStructDefinition_Type = {
	PyObject_HEAD_INIT(0) /* set in initxstruct() */
	0,
	"structdef",
	sizeof(PyStructDefinition),
	0,
	(destructor)PyStructDefinition_dealloc, /*tp_dealloc*/
	0,		/*tp_print*/
	(getattrfunc)PyStructDefinition_getattr, /*tp_getattr*/
	0,		/*tp_setattr*/
	0,		/*tp_compare*/
	0,		/*tp_repr*/
	0,		/*tp_as_number*/
	0,		/*tp_as_sequence*/
	0,		/*tp_as_mapping*/
	0,		/*tp_hash*/
	(ternaryfunc)PyStructDefinition_call, /*tp_call*/
	0,		/*tp_str*/
	0,		/*tp_getattro*/
	0,		/*tp_setattro*/
	0,		/*tp_as_buffer*/
	Py_TPFLAGS_DEFAULT,	/*tp_flags*/
	0,		/*tp_doc*/
};

PyStructDefinition* NewPyStructDefinition()
{
  PyStructDefinition* StructDefinition = 
    PyObject_NEW(PyStructDefinition, &PyStructDefinition_Type);
  if (StructDefinition == NULL)
    return NULL;

  StructDefinition->FieldList = NULL;
  StructDefinition->FieldMap = NULL;
  StructDefinition->InitialStructData = NULL;

  return StructDefinition;
}

static PyStructField* LookupFieldByName(PyStructDefinition* 
  StructDefinition, char* Name)
{
  PyStructField* Field = 
    (PyStructField*) PyDict_GetItemString(StructDefinition->FieldMap, 
      Name);
  if (PyErr_Occurred())
    return NULL;

  if (Field == NULL)
  {
    PyErr_SetString(PyExc_KeyError, Name);
    return NULL;
  }

  return Field;
}

static PyObject* GetFieldValueByName(PyStructDefinition* StructDefinition, 
  char* StructData, char* Name)
{
  PyStructField* Field = LookupFieldByName(StructDefinition, Name);
  if (Field == NULL)
    return NULL;

  return GetFieldValue(Field, StructData);
}

static int SetFieldValueByName(PyStructDefinition* StructDefinition,
  char* StructData, char* Name, PyObject* Value)
{
  PyStructField* Field = LookupFieldByName(StructDefinition, Name);
  if (Field == NULL)
    return -1;

  return SetFieldValue(Field, StructData, Value);
}

static int SetChangeableFieldValueByName(PyStructDefinition* 
  StructDefinition, char* StructData, char* Name, PyObject* Value)
{
  PyStructField* Field = LookupFieldByName(StructDefinition, Name);
  if (Field == NULL)
    return -1;

  if (Field->Changeable)
    return SetFieldValue(Field, StructData, Value);
  else
  {
    PyErr_SetString(StructError, "field is not changeable");
    return -1;
  }
}

static int PrintFields(PyStructDefinition* StructDefinition, 
  char* StructData, FILE* fp, int flags)
{
  int i = 0;
  while (i < PyList_Size(StructDefinition->FieldList))
  {
	  PyStructField* Field = (PyStructField*)
      PyList_GET_ITEM(StructDefinition->FieldList, i); /* borrowed ref */
	  PyObject* Value = GetFieldValue(Field, StructData);
	  if (Value == NULL)
	    return -1;
	  fprintf(fp, "%s: ", PyString_AS_STRING(Field->Name));
    if (PyObject_Print(Value, fp, Py_PRINT_RAW) != 0)
    {
      Py_DECREF(Value);
      return -1;
    }
	  fprintf(fp, "\n");
	  Py_DECREF(Value);
    i++;
  }
  return 0;
}

static PyObject* struct_structdef(PyObject* self, PyObject* args)
{
  char* LayoutSpecifier;
  PyObject* FieldDefinitions;
  PyObject* InitialValues;

  PyStructDefinition* StructDefinition;
  int i;

  if (!PyArg_ParseTuple(args, "sO!", &LayoutSpecifier, &PyList_Type,
      &FieldDefinitions))
    return NULL;

  InitialValues = PyList_New(0);
  if (InitialValues == NULL)
    return NULL;

  StructDefinition = NewPyStructDefinition();
  if (StructDefinition == NULL)
  {
    Py_DECREF(InitialValues);
    return NULL;
  }

  StructDefinition->FormatTable =	whichtable(&LayoutSpecifier);
  if (StructDefinition->FormatTable == NULL)
    goto fail;

  StructDefinition->FieldList = PyList_New(0);
  if (StructDefinition->FieldList == NULL)
    goto fail;

  StructDefinition->FieldMap = PyDict_New();
  if (StructDefinition->FieldMap == NULL)
    goto fail;

  StructDefinition->StructSize = 0;

  i = 0;
  while (i < PyList_Size(FieldDefinitions))
  {
    char* FieldName;
    char* FieldType;
    int RepeatCount = 1;
    PyObject* InitialValue = NULL;
    int Flags = 0;

    char ch;
    formatdef* Format;

    int x;

    PyObject* FieldDefinition = PyList_GET_ITEM(FieldDefinitions, i);
      /* borrowed reference */

    if (!PyArg_ParseTuple(FieldDefinition, "z(si)|Oi", &FieldName,
        &FieldType, &RepeatCount, &InitialValue, &Flags))
      goto fail;

    if (RepeatCount < 0)
    {
      PyErr_SetString(StructError, "invalid repeat count");
      goto fail;
    }

    ch = FieldType[0];
    Format = (formatdef*) getentry(ch, StructDefinition->FormatTable);
    if (Format == NULL)
      goto fail;

    StructDefinition->StructSize = align(StructDefinition->StructSize, ch, Format);

    if ((ch != 'x') && ((RepeatCount != 0) || (ch == 's'))) 
    {
      PyStructField* Field = NewPyStructField();
      if (Field == NULL)
        goto fail;

      if (PyList_Append(StructDefinition->FieldList, (PyObject*) Field) != 0)
      {
        Py_DECREF(Field);
        goto fail;
      }

      /* from now on, Field is owned by StructDefinition */

      Py_DECREF(Field); 

      if (FieldName != NULL) 
      {
        PyObject* CurrentField = PyDict_GetItemString(
          StructDefinition->FieldMap, FieldName); /* borrowed ref */
        if (PyErr_Occurred())
          goto fail;

        if (CurrentField != NULL)
        {
          PyErr_SetString(StructError, "duplicate field name");
          goto fail;
        }

        Field->Name = PyString_FromString(FieldName);
        if (Field->Name == NULL)
          goto fail;

        if (PyDict_SetItemString(StructDefinition->FieldMap, 
            FieldName, (PyObject*) Field) != 0)
          goto fail;
      }

      Field->Changeable = !(Flags & FLAG_READONLY);
      Field->RepeatCount = RepeatCount;
      Field->Format = Format;
      Field->Offset = StructDefinition->StructSize;

      if (InitialValue == NULL)
        InitialValue = Py_None;

      if (PyList_Append(InitialValues, InitialValue) != 0)
        goto fail;
    }
    else /* other combinations do not count as fields */
    {
      if (FieldName != NULL)
      {
        PyErr_SetString(StructError, "field name given to num/format "
          "combination that does not count as a field");
        goto fail;
      }
    }

    x = RepeatCount * Format->size;
	  if (x/Format->size != RepeatCount)
    {
	    PyErr_SetString(StructError, "field size overflow");
		  goto fail;
	  }

    StructDefinition->StructSize += x;
    if (StructDefinition->StructSize < 0)
    {
	    PyErr_SetString(StructError, "struct size overflow");
		  goto fail;
	  }

    i++;
  }

  if (StructDefinition->StructSize == 0)
  {
    PyErr_SetString(StructError, "zero struct size");
	  goto fail;
  }

  StructDefinition->InitialStructData = malloc(StructDefinition->StructSize);
  if (StructDefinition->InitialStructData == NULL)
  {
    PyErr_NoMemory();
    goto fail;
  }

  memset(StructDefinition->InitialStructData, '\0', StructDefinition->StructSize);

  i = 0;
  while (i < PyList_Size(InitialValues))
  {
    PyObject* InitialValue = PyList_GetItem(InitialValues, i);
      /* borrowed reference */
    if (InitialValue == NULL)
      goto fail;

    if (InitialValue != Py_None)
    {
      PyStructField* Field = 
        (PyStructField*) PyList_GetItem(StructDefinition->FieldList, i);
          /* borrowed reference */

      if (Field == NULL)
        goto fail;

      if (SetFieldValue(Field, StructDefinition->InitialStructData, 
          InitialValue) != 0)
        goto fail;
    }

    i++;
  }

  return (PyObject*) StructDefinition;

fail:

  Py_DECREF(StructDefinition);
  Py_DECREF(InitialValues);
  return NULL;
}

/*----------------*/
/* PyStructObject */
/*----------------*/

typedef struct {
  PyObject_HEAD
  PyStructDefinition* StructDefinition;
  char* StructData;
} PyStructObject;

static void PyStructObject_dealloc(PyStructObject* self)
{
  if (self->StructData != NULL)
    free(self->StructData);

  if (self->StructDefinition != NULL)
    Py_DECREF(self->StructDefinition);

  PyMem_DEL(self);
}

static int PyStructObject_print(PyStructObject* self, FILE* fp, int flags)
{
  return PrintFields(self->StructDefinition, self->StructData, fp, flags);
}

static PyObject* PyStructObject_getattr(PyStructObject* self, char* name)
{
  return GetFieldValueByName(self->StructDefinition, self->StructData, name);
}

static int PyStructObject_setattr(PyStructObject* self, char* name, 
  PyObject* value)
{
  if (value == NULL)
  {
    PyErr_SetString(StructError, "attribute can not be deleted");
	  return -1;
  }
  else
    return SetChangeableFieldValueByName(self->StructDefinition,
      self->StructData, name, value);
}

PyObject* PyStructObject_str(PyStructObject* self)
{
  return PyString_FromStringAndSize(self->StructData, 
    self->StructDefinition->StructSize);
}

/* Mapping methods */

static int PyStructObject_length(PyStructObject* self)
{
  return PyDict_Size(self->StructDefinition->FieldMap);
}

static PyObject* PyStructObject_subscript(PyStructObject* self, 
  PyObject* key)
{
  char* str = PyString_AsString(key);
  if (str == NULL)
    return NULL;

  return GetFieldValueByName(self->StructDefinition, self->StructData, 
    str);
}

static int PyStructObject_ass_sub(PyStructObject* self, PyObject* key, 
  PyObject* value)
{
  if (value == NULL)
  {
    PyErr_SetString(StructError, "key can not be deleted");
	  return -1;
  }
  else
  {
    char* str = PyString_AsString(key);
    if (str == NULL)
      return -1;

    return SetChangeableFieldValueByName(self->StructDefinition,
      self->StructData, str, value);
  }
}

static PyMappingMethods PyStructObject_as_mapping = {
	(inquiry)PyStructObject_length, /*mp_length*/
	(binaryfunc)PyStructObject_subscript, /*mp_subscript*/
	(objobjargproc)PyStructObject_ass_sub, /*mp_ass_subscript*/
};

/* Buffer methods */

static int PyStructObject_getreadbuf(PyStructObject* self, 
  int idx, void** pp)
{
  if (idx != 0 ) 
  {
	  PyErr_SetString(PyExc_SystemError,
	    "accessing non-existent buffer segment");
	  return -1;
  }
  *pp = self->StructData;
  return self->StructDefinition->StructSize;
}

static int PyStructObject_getwritebuf(PyStructObject* self, 
  int idx, void** pp)
{
  return PyStructObject_getreadbuf(self, idx, pp);
}

static int PyStructObject_getsegcount(PyStructObject* self, 
  int* lenp)
{
  if (lenp)
    *lenp = self->StructDefinition->StructSize; 
  return 1; /* this is the segment count */
}

static int PyStructObject_getcharbuf(PyStructObject* self, 
  int idx, const char** pp)
{
  return PyStructObject_getreadbuf(self, idx, (void**) pp);
}

static PyBufferProcs PyStructObject_as_buffer = {
  (getreadbufferproc)PyStructObject_getreadbuf,
  (getwritebufferproc)PyStructObject_getwritebuf,
  (getsegcountproc)PyStructObject_getsegcount,
  (getcharbufferproc)PyStructObject_getcharbuf,
};

PyTypeObject PyStructObject_Type = {
    PyObject_HEAD_INIT(0)
	0,
	"structobject",
	sizeof(PyStructObject),
	0,
	(destructor)PyStructObject_dealloc,  /*tp_dealloc*/
	(printfunc)PyStructObject_print,		/*tp_print*/
	(getattrfunc)PyStructObject_getattr, /*tp_getattr*/
	(setattrfunc)PyStructObject_setattr, /*tp_setattr*/
	0,		/*tp_compare*/
	0,		/*tp_repr*/
	0,		/*tp_as_number*/
	0,		/*tp_as_sequence*/
	&PyStructObject_as_mapping,		/*tp_as_mapping*/
	0,		/*tp_hash*/
	0,		/*tp_call*/
	(reprfunc)PyStructObject_str,		/*tp_str*/
	0,		/*tp_getattro*/
	0,		/*tp_setattro*/
	&PyStructObject_as_buffer,		/*tp_as_buffer*/
	Py_TPFLAGS_DEFAULT,	/*tp_flags*/
	0,		/*tp_doc*/
};

static PyObject* NewStructObject(PyStructDefinition* StructDefinition, 
  char* data, int len)
{
  int gap;

  PyStructObject* StructObject = 
    PyObject_NEW(PyStructObject, &PyStructObject_Type);
  if (StructObject == NULL)
    return NULL;

  StructObject->StructDefinition = StructDefinition;
  Py_INCREF(StructDefinition);

  StructObject->StructData = malloc(StructDefinition->StructSize);
  if (StructObject->StructData == NULL)
  {
    PyErr_NoMemory();
	  goto fail;
  }

  gap = StructDefinition->StructSize - len;
  if (gap <= 0)
    memcpy(StructObject->StructData, data, StructDefinition->StructSize);
  else
  {
    memcpy(StructObject->StructData, data, len);
    memset(StructObject->StructData + len, 0, gap);
  }  

  return (PyObject*) StructObject;

fail:

  Py_DECREF(StructObject);
  return NULL;
}

/* Module initialization */

/* List of functions */

static PyMethodDef struct_methods[] = {
	{"calcsize",	struct_calcsize,	METH_VARARGS, calcsize__doc__},
	{"pack",	struct_pack,		METH_VARARGS, pack__doc__},
	{"unpack",	struct_unpack,		METH_VARARGS, unpack__doc__},
	{"structdef",	struct_structdef,	METH_VARARGS },
	{NULL,		NULL}		/* sentinel */
};

/* Module initialization */

typedef struct {
  char* Name;
  char* Value;
} PyStringConstant;

PyStringConstant StructStringConstants[] = {

  /* format table specifiers */

  { "native", "@" },
  { "standard", "=" },
  { "little_endian", "<" },
  { "big_endian", ">" },
  { "network", "!" },

  /* field type specifiers */

  { "pad", "x" },
  { "char", "c" },
  { "signed_char", "b" },
  { "unsigned_char", "B" },
  { "octet", "B" },
  { "short", "h" },
  { "unsigned_short", "H" },
  { "int", "i" },
  { "unsigned_int", "I" },
  { "long", "l" },
  { "unsigned_long", "L" },
  { "float" , "f" },
  { "double", "d" },
  { "string", "s" },
  { "pascal_string", "p" },
  { "pointer", "P" },

  /* sentinel */

  { NULL, NULL }
};

static int InitializeStringConstants(PyObject* SymbolDictionary,
  PyStringConstant* StringConstants)
{
  PyStringConstant* StringConstant = StringConstants;
  while (StringConstant->Name != NULL)
  {
    PyObject* StringObject = PyString_FromString(StringConstant->Value);
    if (StringObject == NULL)
      return -1;
    if (PyDict_SetItemString(SymbolDictionary, StringConstant->Name,
        StringObject) != 0)
    {
      Py_DECREF(StringObject);
      return -1;
    }
    Py_DECREF(StringObject);
    StringConstant++;
  }
  return 0;
}

typedef struct {
  char* Name;
  int Value;
} PyIntegerConstant;

PyIntegerConstant StructIntegerConstants[] = {

  /* flags */

  { "readonly", FLAG_READONLY },

  /* sentinel */

  { NULL, 0 }
};

static int InitializeIntegerConstants(PyObject* SymbolDictionary,
  PyIntegerConstant* IntegerConstants)
{
  PyIntegerConstant* IntegerConstant = IntegerConstants;
  while (IntegerConstant->Name != NULL)
  {
    PyObject* IntegerObject = PyInt_FromLong((long) IntegerConstant->Value);
    if (IntegerObject == NULL)
      return -1;
    if (PyDict_SetItemString(SymbolDictionary, IntegerConstant->Name,
        IntegerObject) != 0)
    {
      Py_DECREF(IntegerObject);
      return -1;
    }
    Py_DECREF(IntegerObject);
    IntegerConstant++;
  }
  return 0;
}

DL_EXPORT(void)
initxstruct()
{
	PyObject *m, *d;

  PyStructField_Type.ob_type = &PyType_Type;
  PyStructDefinition_Type.ob_type = &PyType_Type;
  PyStructObject_Type.ob_type = &PyType_Type;

	/* Create the module and add the functions */
	m = Py_InitModule4("xstruct", struct_methods, struct__doc__,
    (PyObject*)NULL, PYTHON_API_VERSION);

	/* Add some symbolic constants to the module */
	d = PyModule_GetDict(m);
	StructError = PyErr_NewException("xstruct.error", NULL, NULL);
	if (StructError == NULL)
		return;
	PyDict_SetItemString(d, "error", StructError);

  InitializeStringConstants(d, StructStringConstants);
  InitializeIntegerConstants(d, StructIntegerConstants);
}
