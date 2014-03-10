// ACME - a crossassembler for producing 6502/65c02/65816 code.
// Copyright (C) 1998-2014 Marco Baye
// Have a look at "acme.c" for further info
//
// Output stuff
// 24 Nov 2007	Added possibility to suppress segment overlap warnings 
// 25 Sep 2011	Fixed bug in !to (colons in filename could be interpreted as EOS)
//  5 Mar 2014	Fixed bug where setting *>0xffff resulted in hangups.
#include <stdlib.h>
//#include <stdio.h>
#include <string.h>	// for memset()
#include "acme.h"
#include "alu.h"
#include "config.h"
#include "cpu.h"
#include "dynabuf.h"
#include "global.h"
#include "input.h"
#include "output.h"
#include "platform.h"
#include "tree.h"


// constants
#define OUTBUFFERSIZE		65536
#define NO_SEGMENT_START	(-1)	// invalid value to signal "not in a segment"


// structure for linked list of segment data
struct segment {
	struct segment	*next,
			*prev;
	intval_t	start,
			length;
};

// structure for all output stuff:
struct output {
	// output buffer stuff
	char		*buffer;	// holds assembled code
	intval_t	write_idx;	// index of next write
	intval_t	lowest_written;		// smallest address used
	intval_t	highest_written;	// largest address used
	int		initvalue_set;	// actually bool
	struct {
		intval_t	start;	// start of current segment (or NO_SEGMENT_START)
		intval_t	max;	// highest address segment may use
		int		flags;	// "overlay" and "invisible" flags
		struct segment	list_head;	// head element of doubly-linked ring list
	} segment;
};
static struct output	default_output;
static struct output	*out	= &default_output;

// variables

// FIXME - move file format stuff to some other .c file!
// predefined stuff
static struct node_t	*file_format_tree	= NULL;	// tree to hold output formats
// possible file formats
enum out_format_t {
	OUTPUT_FORMAT_UNSPECIFIED,	// default (uses "plain" actually)
	OUTPUT_FORMAT_APPLE,		// load address, length, code
	OUTPUT_FORMAT_CBM,		// load address, code (default for "!to" pseudo opcode)
	OUTPUT_FORMAT_PLAIN		// code only
};
static struct node_t	file_formats[]	= {
	PREDEFNODE("apple",	OUTPUT_FORMAT_APPLE),
	PREDEFNODE(s_cbm,	OUTPUT_FORMAT_CBM),
//	PREDEFNODE("o65",	OUTPUT_FORMAT_O65),
	PREDEFLAST("plain",	OUTPUT_FORMAT_PLAIN),
	//    ^^^^ this marks the last element
};
// chosen file format
static enum out_format_t	output_format	= OUTPUT_FORMAT_UNSPECIFIED;


// predefined stuff
static struct node_t	*segment_modifier_tree	= NULL;	// tree to hold segment modifiers
// segment modifiers
#define	SEGMENT_FLAG_OVERLAY	(1u << 0)
#define	SEGMENT_FLAG_INVISIBLE	(1u << 1)
static struct node_t	segment_modifiers[]	= {
	PREDEFNODE("overlay",	SEGMENT_FLAG_OVERLAY),
	PREDEFLAST("invisible",	SEGMENT_FLAG_INVISIBLE),
	//    ^^^^ this marks the last element
};


// set up new out->segment.max value according to the given address.
// just find the next segment start and subtract 1.
static void find_segment_max(intval_t new_pc)
{
	struct segment	*test_segment	= out->segment.list_head.next;

	// search for smallest segment start address that
	// is larger than given address
	// use list head as sentinel
// FIXME - if +1 overflows intval_t, we have an infinite loop!
	out->segment.list_head.start = new_pc + 1;
	while (test_segment->start <= new_pc)
		test_segment = test_segment->next;
	if (test_segment == &out->segment.list_head)
		out->segment.max = OUTBUFFERSIZE - 1;
	else
		out->segment.max = test_segment->start - 1;	// last free address available
}


//
static void border_crossed(int current_offset)
{
	if (current_offset >= OUTBUFFERSIZE)
		Throw_serious_error("Produced too much code.");
	if (pass_count == 0) {
		Throw_warning("Segment reached another one, overwriting it.");
		find_segment_max(current_offset + 1);	// find new (next) limit
	}
}


// function ptr to write byte into output buffer (might point to real fn or error trigger)
void (*Output_byte)(intval_t byte);


// send low byte to output buffer, automatically increasing program counter
static void real_output(intval_t byte)
{
	// did we reach segment limit?
	if (out->write_idx > out->segment.max)
		border_crossed(out->write_idx);
	// new minimum address?
	if (out->write_idx < out->lowest_written)
		out->lowest_written = out->write_idx;
	// new maximum address?
	if (out->write_idx > out->highest_written)
		out->highest_written = out->write_idx;
	// write byte and advance ptrs
	out->buffer[out->write_idx++] = byte & 0xff;
	CPU_state.add_to_pc++;
}


// throw error (pc undefined) and use fake pc from now on
static void no_output(intval_t byte)
{
	Throw_error(exception_pc_undefined);
	// now change fn ptr to not complain again.
	Output_byte = real_output;
	Output_byte(byte);	// try again
}


// call this if really calling Output_byte would be a waste of time
void Output_fake(int size)
{
	if (size < 1)
		return;

	// check whether ptr undefined
	if (Output_byte == no_output) {
		Output_byte(0);	// trigger error with a dummy byte
		size--;	// fix amount to cater for dummy byte
	}
	// did we reach segment limit?
	if (out->write_idx + size - 1 > out->segment.max)
		border_crossed(out->write_idx + size - 1);
	// new minimum address?
	if (out->write_idx < out->lowest_written)
		out->lowest_written = out->write_idx;
	// new maximum address?
	if (out->write_idx + size - 1 > out->highest_written)
		out->highest_written = out->write_idx + size - 1;
	// advance ptrs
	out->write_idx += size;
	CPU_state.add_to_pc += size;
}


// output 8-bit value with range check
void Output_8b(intval_t value)
{
	if ((value <= 0xff) && (value >= -0x80))
		Output_byte(value);
	else
		Throw_error(exception_number_out_of_range);
}


// output 16-bit value with range check
void Output_16b(intval_t value)
{
	if ((value <= 0xffff) && (value >= -0x8000)) {
		Output_byte(value);
		Output_byte(value >> 8);
	} else {
		Throw_error(exception_number_out_of_range);
	}
}


// output 24-bit value with range check
void Output_24b(intval_t value)
{
	if ((value <= 0xffffff) && (value >= -0x800000)) {
		Output_byte(value);
		Output_byte(value >> 8);
		Output_byte(value >> 16);
	} else {
		Throw_error(exception_number_out_of_range);
	}
}


// output 32-bit value (without range check)
void Output_32b(intval_t value)
{
//  if ((Value <= 0x7fffffff) && (Value >= -0x80000000)) {
	Output_byte(value);
	Output_byte(value >> 8);
	Output_byte(value >> 16);
	Output_byte(value >> 24);
//  } else {
//	Throw_error(exception_number_out_of_range);
//  }
}


// fill output buffer with given byte value
static void fill_completely(char value)
{
	memset(out->buffer, value, OUTBUFFERSIZE);
}


// define default value for empty memory ("!initmem" pseudo opcode)
// FIXME - move to basics.c
static enum eos_t PO_initmem(void)
{
	intval_t	content;

	// ignore in all passes but in first
	if (pass_count)
		return SKIP_REMAINDER;

	// if MemInit flag is already set, complain
	if (out->initvalue_set) {
		Throw_warning("Memory already initialised.");
		return SKIP_REMAINDER;
	}
	// set MemInit flag
	out->initvalue_set = TRUE;
	// get value and init memory
	content = ALU_defined_int();
	if ((content > 0xff) || (content < -0x80))
		Throw_error(exception_number_out_of_range);
	// init memory
	fill_completely(content);
	// enforce another pass
	if (pass_undefined_count == 0)
		pass_undefined_count = 1;
// FIXME - enforcing another pass is not needed if there hasn't been any
// output yet. But that's tricky to detect without too much overhead.
// The old solution was to add &&(out->lowest_written < out->highest_written+1) to "if" above
	return ENSURE_EOS;
}


// try to set output format held in DynaBuf. Returns whether succeeded.
// FIXME - move to basics.c?
int Output_set_output_format(void)
{
	void	*node_body;

	if (!Tree_easy_scan(file_format_tree, &node_body, GlobalDynaBuf))
		return FALSE;

	output_format = (enum out_format_t) node_body;
	return TRUE;
}


// select output file and format ("!to" pseudo opcode)
// FIXME - move to basics.c
static enum eos_t PO_to(void)
{
	// bugfix: first read filename, *then* check for first pass.
	// if skipping right away, quoted colons might be misinterpreted as EOS
	// FIXME - why not just fix the skipping code to handle quotes? :)
	// "!sl" has been fixed as well

	// read filename to global dynamic buffer
	// if no file name given, exit (complaining will have been done)
	if (Input_read_filename(FALSE))
		return SKIP_REMAINDER;

	// only act upon this pseudo opcode in first pass
	if (pass_count)
		return SKIP_REMAINDER;

	// if output file already chosen, complain and exit
	if (output_filename) {
		Throw_warning("Output file already chosen.");
		return SKIP_REMAINDER;
	}

	// get malloc'd copy of filename
	output_filename = DynaBuf_get_copy(GlobalDynaBuf);
	// select output format
	// if no comma found, use default file format
	if (Input_accept_comma() == FALSE) {
		if (output_format == OUTPUT_FORMAT_UNSPECIFIED) {
			output_format = OUTPUT_FORMAT_CBM;
			// output deprecation warning
			Throw_warning("Used \"!to\" without file format indicator. Defaulting to \"cbm\".");
		}
		return ENSURE_EOS;
	}

	// parse output format name
	// if no keyword given, give up
	if (Input_read_and_lower_keyword() == 0)
		return SKIP_REMAINDER;

	if (Output_set_output_format())
		return ENSURE_EOS;	// success

	// error occurred
	Throw_error("Unknown output format.");
	return SKIP_REMAINDER;
}


// pseudo ocpode table
// FIXME - move to basics.c
static struct node_t	pseudo_opcodes[]	= {
	PREDEFNODE("initmem",	PO_initmem),
	PREDEFLAST("to",	PO_to),
	//    ^^^^ this marks the last element
};


// init file format tree (is done early, because it is needed for CLI argument parsing)
// FIXME - move to some other file
void Outputfile_init(void)
{
	Tree_add_table(&file_format_tree, file_formats);
}


// init output struct, register pseudo opcodes (done later)
void Output_init(signed long fill_value)
{
	out->buffer = safe_malloc(OUTBUFFERSIZE);
	if (fill_value == MEMINIT_USE_DEFAULT) {
		fill_value = FILLVALUE_INITIAL;
		out->initvalue_set = FALSE;
	} else {
		out->initvalue_set = TRUE;
	}
	// init output buffer (fill memory with initial value)
	fill_completely(fill_value & 0xff);
	Tree_add_table(&pseudo_opcode_tree, pseudo_opcodes);
	Tree_add_table(&segment_modifier_tree, segment_modifiers);
	// init ring list of segments
	out->segment.list_head.next = &out->segment.list_head;
	out->segment.list_head.prev = &out->segment.list_head;
}


// dump used portion of output buffer into output file
void Output_save_file(FILE *fd)
{
	intval_t	start,
			amount;

	if (out->highest_written < out->lowest_written) {
		// nothing written
		start = 0;	// I could try to use some segment start, but what for?
		amount = 0;
	} else {
		start = out->lowest_written;
		amount = out->highest_written - start + 1;
	}
	if (Process_verbosity)
		printf("Saving %ld (0x%lx) bytes (0x%lx - 0x%lx exclusive).\n",
			amount, amount, start, start + amount);
	// output file header according to file format
	switch (output_format) {
	case OUTPUT_FORMAT_APPLE:
		PLATFORM_SETFILETYPE_APPLE(output_filename);
		// output 16-bit load address in little-endian byte order
		putc(start & 255, fd);
		putc(start >>  8, fd);
		// output 16-bit length in little-endian byte order
		putc(amount & 255, fd);
		putc(amount >>  8, fd);
		break;
	case OUTPUT_FORMAT_UNSPECIFIED:
	case OUTPUT_FORMAT_PLAIN:
		PLATFORM_SETFILETYPE_PLAIN(output_filename);
		break;
	case OUTPUT_FORMAT_CBM:
		PLATFORM_SETFILETYPE_CBM(output_filename);
		// output 16-bit load address in little-endian byte order
		putc(start & 255, fd);
		putc(start >>  8, fd);
	}
	// dump output buffer to file
	fwrite(out->buffer + start, amount, 1, fd);
}


// link segment data into segment ring
static void link_segment(intval_t start, intval_t length)
{
	struct segment	*new_segment,
			*test_segment	= out->segment.list_head.next;

	// init new segment
	new_segment = safe_malloc(sizeof(*new_segment));
	new_segment->start = start;
	new_segment->length = length;
	// use ring head as sentinel
	out->segment.list_head.start = start;
	out->segment.list_head.length = length + 1;	// +1 to make sure sentinel exits loop
	// walk ring to find correct spot
	while ((test_segment->start < new_segment->start)
	|| ((test_segment->start == new_segment->start) && (test_segment->length < new_segment->length)))
		test_segment = test_segment->next;
	// link into ring
	new_segment->next = test_segment;
	new_segment->prev = test_segment->prev;
	new_segment->next->prev = new_segment;
	new_segment->prev->next = new_segment;
}


// check whether given PC is inside segment.
// only call in first pass, otherwise too many warnings might be thrown
static void check_segment(intval_t new_pc)
{
	struct segment	*test_segment	= out->segment.list_head.next;

	// use list head as sentinel
	out->segment.list_head.start = new_pc + 1;	// +1 to make sure sentinel exits loop
	out->segment.list_head.length = 1;
	// search ring for matching entry
	while (test_segment->start <= new_pc) {
		if ((test_segment->start + test_segment->length) > new_pc) {
			Throw_warning("Segment starts inside another one, overwriting it.");
			return;
		}

		test_segment = test_segment->next;
	}
}


// clear segment list and disable output
void Output_passinit(void)
{
//	struct segment	*temp;

//FIXME - why clear ring list in every pass?
// Because later pass shouldn't complain about overwriting the same segment from earlier pass!
// Currently this does not happen because segment checks are only done in first pass. FIXME!
	// delete segment list (and free blocks)
//	while ((temp = segment_list)) {
//		segment_list = segment_list->next;
//		free(temp);
//	}

	// invalidate start and end (first byte actually written will fix them)
	out->lowest_written = OUTBUFFERSIZE - 1;
	out->highest_written = 0;
	// deactivate output - any byte written will trigger error:
	Output_byte = no_output;
	out->write_idx = 0;	// same as pc on pass init!
	out->segment.start = NO_SEGMENT_START;	// TODO - "no active segment" could be made a segment flag!
	out->segment.max = OUTBUFFERSIZE - 1;
	out->segment.flags = 0;
}


// show start and end of current segment
// called whenever a new segment begins, and at end of pass.
void Output_end_segment(void)
{
	intval_t	amount;

	// in later passes, ignore completely
	if (pass_count)
		return;

	// if there is no segment, there is nothing to do
	if (out->segment.start == NO_SEGMENT_START)
		return;

	// ignore "invisible" segments
	if (out->segment.flags & SEGMENT_FLAG_INVISIBLE)
		return;

	// ignore empty segments
	amount = out->write_idx - out->segment.start;
	if (amount == 0)
		return;

	// link to segment list
	link_segment(out->segment.start, amount);
	// announce
	if (Process_verbosity > 1)
		printf("Segment size is %ld (0x%lx) bytes (0x%lx - 0x%lx exclusive).\n",
			amount, amount, out->segment.start, out->write_idx);
}


// change output pointer and enable output
void Output_start_segment(intval_t address_change, int segment_flags)
{
	// properly finalize previous segment (link to list, announce)
	Output_end_segment();

	// calculate start of new segment
	out->write_idx = (out->write_idx + address_change) & 0xffff;
	out->segment.start = out->write_idx;
	out->segment.flags = segment_flags;
	// allow writing to output buffer
	Output_byte = real_output;
	// in first pass, check for other segments and maybe issue warning
	if (pass_count == 0) {
		if (!(segment_flags & SEGMENT_FLAG_OVERLAY))
			check_segment(out->segment.start);
		find_segment_max(out->segment.start);
	}
}


// TODO - add "!skip AMOUNT" pseudo opcode as alternative to "* = * + AMOUNT" (needed for assemble-to-end-address)
// called when "* = EXPRESSION" is parsed
// setting program counter via "* = VALUE"
// FIXME - move to basics.c
void PO_setpc(void)
{
	void		*node_body;
	int		segment_flags	= 0;
	intval_t	new_addr	= ALU_defined_int();

	// check for modifiers
	while (Input_accept_comma()) {
		// parse modifier
		// if no keyword given, give up
		if (Input_read_and_lower_keyword() == 0)
			return;

		if (!Tree_easy_scan(segment_modifier_tree, &node_body, GlobalDynaBuf)) {
			Throw_error("Unknown \"* =\" segment modifier.");
			return;
		}

		segment_flags |= (int) node_body;
	}
	CPU_set_pc(new_addr, segment_flags);
}


