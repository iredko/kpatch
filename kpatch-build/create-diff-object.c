/*
 * create-diff-object.c
 *
 * Copyright (C) 2014 Seth Jennings <sjenning@redhat.com>
 * Copyright (C) 2013-2014 Josh Poimboeuf <jpoimboe@redhat.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA,
 * 02110-1301, USA.
 */

/*
 * This file contains the heart of the ELF object differencing engine.
 *
 * The tool takes two ELF objects from two versions of the same source
 * file; a "base" object and a "patched" object.  These object need to have
 * been compiled with the -ffunction-sections and -fdata-sections GCC options.
 *
 * The tool compares the objects at a section level to determine what
 * sections have changed.  Once a list of changed sections has been generated,
 * various rules are applied to determine any object local sections that
 * are dependencies of the changed section and also need to be included in
 * the output object.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <error.h>
#include <gelf.h>
#include <argp.h>
#include <libgen.h>
#include <unistd.h>

#include "list.h"
#include "lookup.h"
#include "asm/insn.h"
#include "kpatch-patch.h"
#include "kpatch-elf.h"

#define DIFF_FATAL(format, ...) \
({ \
	fprintf(stderr, "ERROR: %s: " format "\n", childobj, ##__VA_ARGS__); \
	error(2, 0, "unreconcilable difference"); \
})

char *childobj;

enum loglevel loglevel = NORMAL;

/*******************
 * Data structures
 * ****************/
struct special_section {
	char *name;
	int (*group_size)(struct kpatch_elf *kelf, int offset);
};

/*************
 * Functions
 * **********/

/*
 * This function detects whether the given symbol is a "special" static local
 * variable (for lack of a better term).
 *
 * Special static local variables should never be correlated and should always
 * be included if they are referenced by an included function.
 */
static int is_special_static(struct symbol *sym)
{
	static char *prefixes[] = {
		"__key.",
		"__warned.",
		"descriptor.",
		"__func__.",
		"_rs.",
		NULL,
	};
	char **prefix;

	if (!sym)
		return 0;

	if (sym->type == STT_SECTION) {
		/* __verbose section contains the descriptor variables */
		if (!strcmp(sym->name, "__verbose"))
			return 1;

		/* otherwise make sure section is bundled */
		if (!sym->sec->sym)
			return 0;

		/* use bundled object/function symbol for matching */
		sym = sym->sec->sym;
	}

	if (sym->type != STT_OBJECT || sym->bind != STB_LOCAL)
		return 0;

	for (prefix = prefixes; *prefix; prefix++)
		if (!strncmp(sym->name, *prefix, strlen(*prefix)))
			return 1;

	return 0;
}

/*
 * This is like strcmp, but for gcc-mangled symbols.  It skips the comparison
 * of any substring which consists of '.' followed by any number of digits.
 */
static int kpatch_mangled_strcmp(char *s1, char *s2)
{
	while (*s1 == *s2) {
		if (!*s1)
			return 0;
		if (*s1 == '.' && isdigit(s1[1])) {
			if (!isdigit(s2[1]))
				return 1;
			while (isdigit(*++s1))
				;
			while (isdigit(*++s2))
				;
		} else {
			s1++;
			s2++;
		}
	}
	return 1;
}

int rela_equal(struct rela *rela1, struct rela *rela2)
{
	if (rela1->type != rela2->type ||
	    rela1->offset != rela2->offset)
		return 0;

	if (rela1->string)
		return rela2->string && !strcmp(rela1->string, rela2->string);

	if (rela1->addend != rela2->addend)
		return 0;

	if (is_special_static(rela1->sym))
		return !kpatch_mangled_strcmp(rela1->sym->name,
					      rela2->sym->name);

	return !strcmp(rela1->sym->name, rela2->sym->name);
}

void kpatch_compare_correlated_rela_section(struct section *sec)
{
	struct rela *rela1, *rela2 = NULL;

	rela2 = list_entry(sec->twin->relas.next, struct rela, list);
	list_for_each_entry(rela1, &sec->relas, list) {
		if (rela_equal(rela1, rela2)) {
			rela2 = list_entry(rela2->list.next, struct rela, list);
			continue;
		}
		sec->status = CHANGED;
		return;
	}

	sec->status = SAME;
}

void kpatch_compare_correlated_nonrela_section(struct section *sec)
{
	struct section *sec1 = sec, *sec2 = sec->twin;

	if (sec1->sh.sh_type != SHT_NOBITS &&
	    memcmp(sec1->data->d_buf, sec2->data->d_buf, sec1->data->d_size))
		sec->status = CHANGED;
	else
		sec->status = SAME;
}

void kpatch_compare_correlated_section(struct section *sec)
{
	struct section *sec1 = sec, *sec2 = sec->twin;

	/* Compare section headers (must match or fatal) */
	if (sec1->sh.sh_type != sec2->sh.sh_type ||
	    sec1->sh.sh_flags != sec2->sh.sh_flags ||
	    sec1->sh.sh_addr != sec2->sh.sh_addr ||
	    sec1->sh.sh_addralign != sec2->sh.sh_addralign ||
	    sec1->sh.sh_entsize != sec2->sh.sh_entsize)
		DIFF_FATAL("%s section header details differ", sec1->name);

	/* Short circuit for mcount sections, we rebuild regardless */
	if (!strcmp(sec->name, ".rela__mcount_loc") ||
	    !strcmp(sec->name, "__mcount_loc")) {
		sec->status = SAME;
		goto out;
	}

	if (sec1->sh.sh_size != sec2->sh.sh_size ||
	    sec1->data->d_size != sec2->data->d_size) {
		sec->status = CHANGED;
		goto out;
	}

	if (is_rela_section(sec))
		kpatch_compare_correlated_rela_section(sec);
	else
		kpatch_compare_correlated_nonrela_section(sec);
out:
	if (sec->status == CHANGED)
		log_debug("section %s has changed\n", sec->name);
}

/*
 * Determine if a section has changed only due to a WARN* macro call's
 * embedding of the line number into an instruction operand.
 *
 * Warning: Hackery lies herein.  It's hopefully justified by the fact that
 * this issue is very common.
 *
 * Example:
 *
 *  938:   be 70 00 00 00          mov    $0x70,%esi
 *  93d:   48 c7 c7 00 00 00 00    mov    $0x0,%rdi
 *                         940: R_X86_64_32S       .rodata.tcp_conn_request.str1.8+0x88
 *  944:   c6 05 00 00 00 00 01    movb   $0x1,0x0(%rip)        # 94b <tcp_conn_request+0x94b>
 *                         946: R_X86_64_PC32      .data.unlikely-0x1
 *  94b:   e8 00 00 00 00          callq  950 <tcp_conn_request+0x950>
 *                         94c: R_X86_64_PC32      warn_slowpath_null-0x4
 *
 * The pattern which applies to all cases:
 * 1) immediate move of the line number to %esi
 * 2) (optional) string section rela
 * 3) (optional) __warned.xxxxx static local rela
 * 4) warn_slowpath_* rela
 */
static int kpatch_warn_only_change(struct section *sec)
{
	struct insn insn1, insn2;
	unsigned long start1, start2, size, offset, length;
	struct rela *rela;
	int warnonly = 0, found;

	if (sec->status != CHANGED ||
	    is_rela_section(sec) ||
	    !is_text_section(sec) ||
	    sec->sh.sh_size != sec->twin->sh.sh_size ||
	    !sec->rela ||
	    sec->rela->status != SAME)
		return 0;

	start1 = (unsigned long)sec->twin->data->d_buf;
	start2 = (unsigned long)sec->data->d_buf;
	size = sec->sh.sh_size;
	for (offset = 0; offset < size; offset += length) {
		insn_init(&insn1, (void *)(start1 + offset), 1);
		insn_init(&insn2, (void *)(start2 + offset), 1);
		insn_get_length(&insn1);
		insn_get_length(&insn2);
		length = insn1.length;

		if (!insn1.length || !insn2.length)
			ERROR("can't decode instruction in section %s at offset 0x%lx",
			      sec->name, offset);

		if (insn1.length != insn2.length)
			return 0;

		if (!memcmp((void *)start1 + offset, (void *)start2 + offset,
			    length))
			continue;

		/* verify it's a mov immediate to %esi */
		insn_get_opcode(&insn1);
		insn_get_opcode(&insn2);
		if (insn1.opcode.value != 0xbe || insn2.opcode.value != 0xbe)
			return 0;

		/*
		 * Verify zero or more string relas followed by a
		 * warn_slowpath_* rela.
		 */
		found = 0;
		list_for_each_entry(rela, &sec->rela->relas, list) {
			if (rela->offset < offset + length)
				continue;
			if (rela->string)
				continue;
			if (!strncmp(rela->sym->name, "__warned.", 9))
				continue;
			if (!strncmp(rela->sym->name, "warn_slowpath_", 14)) {
				found = 1;
				break;
			}
			return 0;
		}
		if (!found)
			return 0;

		warnonly = 1;
	}

	if (!warnonly)
		ERROR("no instruction changes detected for changed section %s",
		      sec->name);

	return 1;
}

void kpatch_compare_sections(struct list_head *seclist)
{
	struct section *sec;

	/* compare all sections */
	list_for_each_entry(sec, seclist, list) {
		if (sec->twin)
			kpatch_compare_correlated_section(sec);
		else
			sec->status = NEW;
	}

	/* exclude WARN-only changes */
	list_for_each_entry(sec, seclist, list) {
		if (kpatch_warn_only_change(sec)) {
			log_debug("reverting WARN-only section %s status to SAME\n",
				  sec->name);
			sec->status = SAME;
		}
	}

	/* sync symbol status */
	list_for_each_entry(sec, seclist, list) {
		if (is_rela_section(sec)) {
			if (sec->base->sym && sec->base->sym->status != CHANGED)
				sec->base->sym->status = sec->status;
		} else {
			if (sec->sym && sec->sym->status != CHANGED)
				sec->sym->status = sec->status;
		}
	}
}

void kpatch_compare_correlated_symbol(struct symbol *sym)
{
	struct symbol *sym1 = sym, *sym2 = sym->twin;

	if (sym1->sym.st_info != sym2->sym.st_info ||
	    sym1->sym.st_other != sym2->sym.st_other ||
	    (sym1->sec && !sym2->sec) ||
	    (sym2->sec && !sym1->sec))
		DIFF_FATAL("symbol info mismatch: %s", sym1->name);

	/*
	 * If two symbols are correlated but their sections are not, then the
	 * symbol has changed sections.  This is only allowed if the symbol is
	 * moving out of an ignored section.
	 */
	if (sym1->sec && sym2->sec && sym1->sec->twin != sym2->sec) {
		if (sym2->sec->twin && sym2->sec->twin->ignore)
			sym->status = CHANGED;
		else
			DIFF_FATAL("symbol changed sections: %s", sym1->name);
	}

	if (sym1->type == STT_OBJECT &&
	    sym1->sym.st_size != sym2->sym.st_size)
		DIFF_FATAL("object size mismatch: %s", sym1->name);

	if (sym1->sym.st_shndx == SHN_UNDEF ||
	     sym1->sym.st_shndx == SHN_ABS)
		sym1->status = SAME;

	/*
	 * The status of LOCAL symbols is dependent on the status of their
	 * matching section and is set during section comparison.
	 */
}

void kpatch_compare_symbols(struct list_head *symlist)
{
	struct symbol *sym;

	list_for_each_entry(sym, symlist, list) {
		if (sym->twin)
			kpatch_compare_correlated_symbol(sym);
		else
			sym->status = NEW;

		log_debug("symbol %s is %s\n", sym->name, status_str(sym->status));
	}
}

void kpatch_correlate_sections(struct list_head *seclist1, struct list_head *seclist2)
{
	struct section *sec1, *sec2;

	list_for_each_entry(sec1, seclist1, list) {
		list_for_each_entry(sec2, seclist2, list) {
			if (strcmp(sec1->name, sec2->name))
				continue;

			if (is_special_static(is_rela_section(sec1) ?
					      sec1->base->secsym :
					      sec1->secsym))
				continue;

			/*
			 * Group sections must match exactly to be correlated.
			 * Changed group sections are currently not supported.
			 */
			if (sec1->sh.sh_type == SHT_GROUP) {
				if (sec1->data->d_size != sec2->data->d_size)
					continue;
				if (memcmp(sec1->data->d_buf, sec2->data->d_buf,
				           sec1->data->d_size))
					continue;
			}
			sec1->twin = sec2;
			sec2->twin = sec1;
			/* set initial status, might change */
			sec1->status = sec2->status = SAME;
			break;
		}
	}
}

void kpatch_correlate_symbols(struct list_head *symlist1, struct list_head *symlist2)
{
	struct symbol *sym1, *sym2;

	list_for_each_entry(sym1, symlist1, list) {
		list_for_each_entry(sym2, symlist2, list) {
			if (strcmp(sym1->name, sym2->name) ||
			    sym1->type != sym2->type)
				continue;

			if (is_special_static(sym1))
				continue;

			/* group section symbols must have correlated sections */
			if (sym1->sec &&
			    sym1->sec->sh.sh_type == SHT_GROUP &&
			    sym1->sec->twin != sym2->sec)
				continue;

			sym1->twin = sym2;
			sym2->twin = sym1;
			/* set initial status, might change */
			sym1->status = sym2->status = SAME;
			break;
		}
	}
}

void kpatch_compare_elf_headers(Elf *elf1, Elf *elf2)
{
	GElf_Ehdr eh1, eh2;

	if (!gelf_getehdr(elf1, &eh1))
		ERROR("gelf_getehdr");

	if (!gelf_getehdr(elf2, &eh2))
		ERROR("gelf_getehdr");

	if (memcmp(eh1.e_ident, eh2.e_ident, EI_NIDENT) ||
	    eh1.e_type != eh2.e_type ||
	    eh1.e_machine != eh2.e_machine ||
	    eh1.e_version != eh2.e_version ||
	    eh1.e_entry != eh2.e_entry ||
	    eh1.e_phoff != eh2.e_phoff ||
	    eh1.e_flags != eh2.e_flags ||
	    eh1.e_ehsize != eh2.e_ehsize ||
	    eh1.e_phentsize != eh2.e_phentsize ||
	    eh1.e_shentsize != eh2.e_shentsize)
		DIFF_FATAL("ELF headers differ");
}

void kpatch_check_program_headers(Elf *elf)
{
	size_t ph_nr;

	if (elf_getphdrnum(elf, &ph_nr))
		ERROR("elf_getphdrnum");

	if (ph_nr != 0)
		DIFF_FATAL("ELF contains program header");
}

void kpatch_mark_grouped_sections(struct kpatch_elf *kelf)
{
	struct section *groupsec, *sec;
	unsigned int *data, *end;

	list_for_each_entry(groupsec, &kelf->sections, list) {
		if (groupsec->sh.sh_type != SHT_GROUP)
			continue;
		data = groupsec->data->d_buf;
		end = groupsec->data->d_buf + groupsec->data->d_size;
		data++; /* skip first flag word (e.g. GRP_COMDAT) */
		while (data < end) {
			sec = find_section_by_index(&kelf->sections, *data);
			if (!sec)
				ERROR("group section not found");
			sec->grouped = 1;
			log_debug("marking section %s (%d) as grouped\n",
			          sec->name, sec->index);
			data++;
		}
	}
}

/*
 * When gcc makes compiler optimizations which affect a function's calling
 * interface, it mangles the function's name.  For example, sysctl_print_dir is
 * renamed to sysctl_print_dir.isra.2.  The problem is that the trailing number
 * is chosen arbitrarily, and the patched version of the function may end up
 * with a different trailing number.  Rename any mangled patched functions to
 * match their base counterparts.
 */
void kpatch_rename_mangled_functions(struct kpatch_elf *base,
				     struct kpatch_elf *patched)
{
	struct symbol *sym, *basesym;
	char name[256], *origname;
	struct section *sec, *basesec;
	int found;

	list_for_each_entry(sym, &patched->symbols, list) {
		if (sym->type != STT_FUNC)
			continue;

		if (!strstr(sym->name, ".isra.") &&
		    !strstr(sym->name, ".constprop.") &&
		    !strstr(sym->name, ".part."))
			continue;

		found = 0;
		list_for_each_entry(basesym, &base->symbols, list) {
			if (!kpatch_mangled_strcmp(basesym->name, sym->name)) {
				found = 1;
				break;
			}
		}

		if (!found)
			continue;

		if (!strcmp(sym->name, basesym->name))
			continue;

		log_debug("renaming %s to %s\n", sym->name, basesym->name);
		origname = sym->name;
		sym->name = strdup(basesym->name);

		if (sym != sym->sec->sym)
			continue;

		sym->sec->name = strdup(basesym->sec->name);
		if (sym->sec->rela)
			sym->sec->rela->name = strdup(basesym->sec->rela->name);

		/*
		 * When function foo.isra.1 has a switch statement, it might
		 * have a corresponding bundled .rodata.foo.isra.1 section (in
		 * addition to .text.foo.isra.1 which we renamed above).
		 */
		sprintf(name, ".rodata.%s", origname);
		sec = find_section_by_name(&patched->sections, name);
		if (!sec)
			continue;
		sprintf(name, ".rodata.%s", basesym->name);
		basesec = find_section_by_name(&base->sections, name);
		if (!basesec)
			continue;
		sec->name = strdup(basesec->name);
		sec->secsym->name = sec->name;
		if (sec->rela)
			sec->rela->name = strdup(basesec->rela->name);
	}
}

static char *kpatch_section_function_name(struct section *sec)
{
	if (is_rela_section(sec))
		sec = sec->base;
	return sec->sym ? sec->sym->name : sec->name;
}

/*
 * Given a static local variable symbol and a rela section which references it
 * in the base object, find a corresponding usage of a similarly named symbol
 * in the patched object.
 */
static struct symbol *kpatch_find_static_twin(struct section *sec,
					      struct symbol *sym)
{
	struct rela *rela;

	if (!sec->twin)
		return NULL;

	/* find the patched object's corresponding variable */
	list_for_each_entry(rela, &sec->twin->relas, list) {

		if (rela->sym->twin)
			continue;

		if (kpatch_mangled_strcmp(rela->sym->name, sym->name))
			continue;

		return rela->sym;
	}

	return NULL;
}

static int kpatch_is_normal_static_local(struct symbol *sym)
{
	if (sym->type != STT_OBJECT || sym->bind != STB_LOCAL)
		return 0;

	if (!strchr(sym->name, '.'))
		return 0;

	if (is_special_static(sym))
		return 0;

	return 1;
}

/*
 * gcc renames static local variables by appending a period and a number.  For
 * example, __foo could be renamed to __foo.31452.  Unfortunately this number
 * can arbitrarily change.  Correlate them by comparing which functions
 * reference them, and rename the patched symbols to match the base symbol
 * names.
 *
 * Some surprising facts about static local variable symbols:
 *
 * - It's possible for multiple functions to use the same
 *   static local variable if the variable is defined in an
 *   inlined function.
 *
 * - It's also possible for multiple static local variables
 *   with the same name to be used in the same function if they
 *   have different scopes.  (We have to assume that in such
 *   cases, the order in which they're referenced remains the
 *   same between the base and patched objects, as there's no
 *   other way to distinguish them.)
 *
 * - Static locals are usually referenced by functions, but
 *   they can occasionally be referenced by data sections as
 *   well.
 */
void kpatch_correlate_static_local_variables(struct kpatch_elf *base,
					     struct kpatch_elf *patched)
{
	struct symbol *sym, *patched_sym;
	struct section *sec;
	struct rela *rela, *rela2;
	int bundled, patched_bundled, found;

	/*
	 * First undo the correlations for all static locals.  Two static
	 * locals can have the same numbered suffix in the base and patched
	 * objects by coincidence.
	 */
	list_for_each_entry(sym, &base->symbols, list) {

		if (!kpatch_is_normal_static_local(sym))
			continue;

		if (sym->twin) {
			sym->twin->twin = NULL;
			sym->twin = NULL;
		}

		bundled = sym == sym->sec->sym;
		if (bundled && sym->sec->twin) {
			sym->sec->twin->twin = NULL;
			sym->sec->twin = NULL;

			sym->sec->secsym->twin->twin = NULL;
			sym->sec->secsym->twin = NULL;

			if (sym->sec->rela) {
				sym->sec->rela->twin->twin = NULL;
				sym->sec->rela->twin = NULL;
			}
		}
	}

	/*
	 * Do the correlations: for each section reference to a static local,
	 * look for a corresponding reference in the section's twin.
	 */
	list_for_each_entry(sec, &base->sections, list) {

		if (!is_rela_section(sec) ||
		    is_debug_section(sec))
			continue;

		list_for_each_entry(rela, &sec->relas, list) {

			sym = rela->sym;
			if (!kpatch_is_normal_static_local(sym))
				continue;

			if (sym->twin)
				continue;

			bundled = sym == sym->sec->sym;
			if (bundled && sym->sec == sec->base) {
				/*
				 * A rare case where a static local data
				 * structure references itself.  There's no
				 * reliable way to correlate this.  Hopefully
				 * there's another reference to the symbol
				 * somewhere that can be used.
				 */
				log_debug("can't correlate static local %s's reference to itself\n",
					  sym->name);
				continue;
			}

			patched_sym = kpatch_find_static_twin(sec, sym);
			if (!patched_sym)
				DIFF_FATAL("reference to static local variable %s in %s was removed",
					   sym->name,
					   kpatch_section_function_name(sec));

			patched_bundled = patched_sym == patched_sym->sec->sym;
			if (bundled != patched_bundled)
				ERROR("bundle mismatch for symbol %s", sym->name);
			if (!bundled && sym->sec->twin != patched_sym->sec)
				ERROR("sections %s and %s aren't correlated",
				      sym->sec->name, patched_sym->sec->name);

			log_debug("renaming and correlating static local %s to %s\n",
				  patched_sym->name, sym->name);

			patched_sym->name = strdup(sym->name);
			sym->twin = patched_sym;
			patched_sym->twin = sym;

			if (bundled) {
				sym->sec->twin = patched_sym->sec;
				patched_sym->sec->twin = sym->sec;

				sym->sec->secsym->twin = patched_sym->sec->secsym;
				patched_sym->sec->secsym->twin = sym->sec->secsym;

				if (sym->sec->rela && patched_sym->sec->rela) {
					sym->sec->rela->twin = patched_sym->sec->rela;
					patched_sym->sec->rela->twin = sym->sec->rela;
				}
			}
		}
	}

	/*
	 * Make sure that:
	 *
	 * 1. all the base object's referenced static locals have been
	 *    correlated; and
	 *
	 * 2. each reference to a static local in the base object has a
	 *    corresponding reference in the patched object (because a static
	 *    local can be referenced by more than one section).
	 */
	list_for_each_entry(sec, &base->sections, list) {

		if (!is_rela_section(sec) ||
		    is_debug_section(sec))
			continue;

		list_for_each_entry(rela, &sec->relas, list) {

			sym = rela->sym;
			if (!kpatch_is_normal_static_local(sym))
				continue;

			if (!sym->twin || !sec->twin)
				DIFF_FATAL("reference to static local variable %s in %s was removed",
					   sym->name,
					   kpatch_section_function_name(sec));

			found = 0;
			list_for_each_entry(rela2, &sec->twin->relas, list) {
				if (rela2->sym == sym->twin) {
					found = 1;
					break;
				}
			}

			if (!found)
				DIFF_FATAL("static local %s has been correlated with %s, but patched %s is missing a reference to it",
					   sym->name, sym->twin->name,
					   kpatch_section_function_name(sec->twin));
		}
	}

	/*
	 * Now go through the patched object and look for any uncorrelated
	 * static locals to see if we need to print any warnings about new
	 * variables.
	 */
	list_for_each_entry(sec, &patched->sections, list) {

		if (!is_rela_section(sec) ||
		    is_debug_section(sec))
			continue;

		list_for_each_entry(rela, &sec->relas, list) {

			sym = rela->sym;
			if (!kpatch_is_normal_static_local(sym))
				continue;

			if (sym->twin)
				continue;

			log_normal("WARNING: unable to correlate static local variable %s used by %s, assuming variable is new\n",
				   sym->name,
				   kpatch_section_function_name(sec));
			return;
		}
	}
}

void kpatch_correlate_elfs(struct kpatch_elf *kelf1, struct kpatch_elf *kelf2)
{
	kpatch_correlate_sections(&kelf1->sections, &kelf2->sections);
	kpatch_correlate_symbols(&kelf1->symbols, &kelf2->symbols);
}

void kpatch_compare_correlated_elements(struct kpatch_elf *kelf)
{
	/* lists are already correlated at this point */
	kpatch_compare_sections(&kelf->sections);
	kpatch_compare_symbols(&kelf->symbols);
}

void rela_insn(struct section *sec, struct rela *rela, struct insn *insn)
{
	unsigned long insn_addr, start, end, rela_addr;

	start = (unsigned long)sec->base->data->d_buf;
	end = start + sec->base->sh.sh_size;
	rela_addr = start + rela->offset;
	for (insn_addr = start; insn_addr < end; insn_addr += insn->length) {
		insn_init(insn, (void *)insn_addr, 1);
		insn_get_length(insn);
		if (!insn->length)
			ERROR("can't decode instruction in section %s at offset 0x%lx",
			      sec->base->name, insn_addr);
		if (rela_addr >= insn_addr &&
		    rela_addr < insn_addr + insn->length)
			return;
	}
}

/*
 * Mangle the relas a little.  The compiler will sometimes use section symbols
 * to reference local objects and functions rather than the object or function
 * symbols themselves.  We substitute the object/function symbols for the
 * section symbol in this case so that the relas can be properly correlated and
 * so that the existing object/function in vmlinux can be linked to.
 */
void kpatch_replace_sections_syms(struct kpatch_elf *kelf)
{
	struct section *sec;
	struct rela *rela;
	struct symbol *sym;
	int add_off;

	log_debug("\n");

	list_for_each_entry(sec, &kelf->sections, list) {
		if (!is_rela_section(sec) ||
		    is_debug_section(sec))
			continue;

		list_for_each_entry(rela, &sec->relas, list) {

			if (rela->sym->type != STT_SECTION)
				continue;

			/*
			 * Replace references to bundled sections with their
			 * symbols.
			 */
			if (rela->sym->sec && rela->sym->sec->sym) {
				rela->sym = rela->sym->sec->sym;
				continue;
			}

			if (rela->type == R_X86_64_PC32) {
				struct insn insn;
				rela_insn(sec, rela, &insn);
				add_off = (long)insn.next_byte -
					  (long)sec->base->data->d_buf -
					  rela->offset;
			} else if (rela->type == R_X86_64_64 ||
				   rela->type == R_X86_64_32S)
				add_off = 0;
			else
				continue;

			/*
			 * Attempt to replace references to unbundled sections
			 * with their symbols.
			 */
			list_for_each_entry(sym, &kelf->symbols, list) {
				int start, end;

				if (sym->type == STT_SECTION ||
				    sym->sec != rela->sym->sec)
					continue;

				start = sym->sym.st_value;
				end = sym->sym.st_value + sym->sym.st_size;

				if (!is_text_section(sym->sec) &&
				    rela->type == R_X86_64_32S &&
				    rela->addend == sym->sec->sh.sh_size &&
				    end == sym->sec->sh.sh_size) {

					/*
					 * A special case where gcc needs a
					 * pointer to the address at the end of
					 * a data section.
					 *
					 * This is usually used with a compare
					 * instruction to determine when to end
					 * a loop.  The code doesn't actually
					 * dereference the pointer so this is
					 * "normal" and we just replace the
					 * section reference with a reference
					 * to the last symbol in the section.
					 *
					 * Note that this only catches the
					 * issue when it happens at the end of
					 * a section.  It can also happen in
					 * the middle of a section.  In that
					 * case, the wrong symbol will be
					 * associated with the reference.  But
					 * that's ok because:
					 *
					 * 1) This situation only occurs when
					 *    gcc is trying to get the address
					 *    of the symbol, not the contents
					 *    of its data; and
					 *
					 * 2) Because kpatch doesn't allow data
					 *    sections to change,
					 *    &(var1+sizeof(var1)) will always
					 *    be the same as &var2.
					 */

				} else if (rela->addend + add_off < start ||
					   rela->addend + add_off >= end)
					continue;

				log_debug("%s: replacing %s+%d reference with %s+%d\n",
					  sec->name,
					  rela->sym->name, rela->addend,
					  sym->name, rela->addend - start);

				rela->sym = sym;
				rela->addend -= start;
				break;
			}
		}
	}
	log_debug("\n");
}

static void kpatch_check_fentry_calls(struct kpatch_elf *kelf)
{
	struct symbol *sym;
	int errs = 0;

	list_for_each_entry(sym, &kelf->symbols, list) {
		if (sym->type != STT_FUNC || sym->status != CHANGED)
			continue;
		if (!sym->twin->has_fentry_call) {
			log_normal("function %s has no fentry call, unable to patch\n",
				   sym->name);
			errs++;
		}
	}

	if (errs)
		DIFF_FATAL("%d function(s) can not be patched", errs);
}

void kpatch_verify_patchability(struct kpatch_elf *kelf)
{
	struct section *sec;
	int errs = 0;

	list_for_each_entry(sec, &kelf->sections, list) {
		if (sec->status == CHANGED && !sec->include) {
			log_normal("changed section %s not selected for inclusion\n",
				   sec->name);
			errs++;
		}

		if (sec->status != SAME && sec->grouped) {
			log_normal("changed section %s is part of a section group\n",
				   sec->name);
			errs++;
		}

		if (sec->sh.sh_type == SHT_GROUP && sec->status == NEW) {
			log_normal("new/changed group sections are not supported\n");
			errs++;
		}

		/*
		 * ensure we aren't including .data.* or .bss.*
		 * (.data.unlikely is ok b/c it only has __warned vars)
		 */
		if (sec->include && sec->status != NEW &&
		    (!strncmp(sec->name, ".data", 5) ||
		     !strncmp(sec->name, ".bss", 4)) &&
		    strcmp(sec->name, ".data.unlikely")) {
			log_normal("data section %s selected for inclusion\n",
				   sec->name);
			errs++;
		}
	}

	if (errs)
		DIFF_FATAL("%d unsupported section change(s)", errs);
}

#define inc_printf(fmt, ...) \
	log_debug("%*s" fmt, recurselevel, "", ##__VA_ARGS__);

void kpatch_include_symbol(struct symbol *sym, int recurselevel)
{
	struct rela *rela;
	struct section *sec;

	inc_printf("start include_symbol(%s)\n", sym->name);
	sym->include = 1;
	inc_printf("symbol %s is included\n", sym->name);
	/*
	 * Check if sym is a non-local symbol (sym->sec is NULL) or
	 * if an unchanged local symbol.  This a base case for the
	 * inclusion recursion.
	 */
	if (!sym->sec || sym->sec->include ||
	    (sym->type != STT_SECTION && sym->status == SAME))
		goto out;
	sec = sym->sec;
	sec->include = 1;
	inc_printf("section %s is included\n", sec->name);
	if (sec->secsym && sec->secsym != sym) {
		sec->secsym->include = 1;
		inc_printf("section symbol %s is included\n", sec->secsym->name);
	}
	if (!sec->rela)
		goto out;
	sec->rela->include = 1;
	inc_printf("section %s is included\n", sec->rela->name);
	list_for_each_entry(rela, &sec->rela->relas, list)
		kpatch_include_symbol(rela->sym, recurselevel+1);
out:
	inc_printf("end include_symbol(%s)\n", sym->name);
	return;
}

void kpatch_include_standard_elements(struct kpatch_elf *kelf)
{
	struct section *sec;

	list_for_each_entry(sec, &kelf->sections, list) {
		/* include these sections even if they haven't changed */
		if (!strcmp(sec->name, ".shstrtab") ||
		    !strcmp(sec->name, ".strtab") ||
		    !strcmp(sec->name, ".symtab") ||
		    !strncmp(sec->name, ".rodata.str1.", 13)) {
			sec->include = 1;
			if (sec->secsym)
				sec->secsym->include = 1;
		}
	}

	/* include the NULL symbol */
	list_entry(kelf->symbols.next, struct symbol, list)->include = 1;
}

int kpatch_include_hook_elements(struct kpatch_elf *kelf)
{
	struct section *sec;
	struct symbol *sym;
	struct rela *rela;
	int found = 0;

	/* include load/unload sections */
	list_for_each_entry(sec, &kelf->sections, list) {
		if (!strcmp(sec->name, ".kpatch.hooks.load") ||
		    !strcmp(sec->name, ".kpatch.hooks.unload") ||
		    !strcmp(sec->name, ".rela.kpatch.hooks.load") ||
		    !strcmp(sec->name, ".rela.kpatch.hooks.unload")) {
			sec->include = 1;
			found = 1;
			if (is_rela_section(sec)) {
				/* include hook dependencies */
				rela = list_entry(sec->relas.next,
			                         struct rela, list);
				sym = rela->sym;
				log_normal("found hook: %s\n",sym->name);
				kpatch_include_symbol(sym, 0);
				/* strip the hook symbol */
				sym->include = 0;
				sym->sec->sym = NULL;
				/* use section symbol instead */
				rela->sym = sym->sec->secsym;
			} else {
				sec->secsym->include = 1;
			}
		}
	}

	/*
	 * Strip temporary global load/unload function pointer objects
	 * used by the kpatch_[load|unload]() macros.
	 */
	list_for_each_entry(sym, &kelf->symbols, list)
		if (!strcmp(sym->name, "kpatch_load_data") ||
		    !strcmp(sym->name, "kpatch_unload_data"))
			sym->include = 0;

	return found;
}

void kpatch_include_force_elements(struct kpatch_elf *kelf)
{
	struct section *sec;
	struct symbol *sym;
	struct rela *rela;

	/* include force sections */
	list_for_each_entry(sec, &kelf->sections, list) {
		if (!strcmp(sec->name, ".kpatch.force") ||
		    !strcmp(sec->name, ".rela.kpatch.force")) {
			sec->include = 1;
			if (!is_rela_section(sec)) {
				/* .kpatch.force */
				sec->secsym->include = 1;
				continue;
			}
			/* .rela.kpatch.force */
			list_for_each_entry(rela, &sec->relas, list)
				log_normal("function '%s' marked with KPATCH_FORCE_UNSAFE!\n",
				           rela->sym->name);
		}
	}

	/* strip temporary global kpatch_force_func_* symbols */
	list_for_each_entry(sym, &kelf->symbols, list)
		if (!strncmp(sym->name, "__kpatch_force_func_",
		            strlen("__kpatch_force_func_")))
			sym->include = 0;
}

int kpatch_include_new_globals(struct kpatch_elf *kelf)
{
	struct symbol *sym;
	int nr = 0;

	list_for_each_entry(sym, &kelf->symbols, list) {
		if (sym->bind == STB_GLOBAL && sym->sec &&
		    sym->status == NEW) {
			kpatch_include_symbol(sym, 0);
			nr++;
		}
	}

	return nr;
}

int kpatch_include_changed_functions(struct kpatch_elf *kelf)
{
	struct symbol *sym;
	int changed_nr = 0;

	log_debug("\n=== Inclusion Tree ===\n");

	list_for_each_entry(sym, &kelf->symbols, list) {
		if (sym->status == CHANGED &&
		    sym->type == STT_FUNC) {
			changed_nr++;
			kpatch_include_symbol(sym, 0);
		}

		if (sym->type == STT_FILE)
			sym->include = 1;
	}

	return changed_nr;
}

void kpatch_print_changes(struct kpatch_elf *kelf)
{
	struct symbol *sym;

	list_for_each_entry(sym, &kelf->symbols, list) {
		if (!sym->include || !sym->sec || sym->type != STT_FUNC)
			continue;
		if (sym->status == NEW)
			log_normal("new function: %s\n", sym->name);
		else if (sym->status == CHANGED)
			log_normal("changed function: %s\n", sym->name);
	}
}

void kpatch_migrate_symbols(struct list_head *src,
                                    struct list_head *dst,
                                    int (*select)(struct symbol *))
{
	struct symbol *sym, *safe;

	list_for_each_entry_safe(sym, safe, src, list) {
		if (select && !select(sym))
			continue;

		list_del(&sym->list);
		list_add_tail(&sym->list, dst);
	}
}

void kpatch_migrate_included_elements(struct kpatch_elf *kelf, struct kpatch_elf **kelfout)
{
	struct section *sec, *safesec;
	struct symbol *sym, *safesym;
	struct kpatch_elf *out;

	/* allocate output kelf */
	out = malloc(sizeof(*out));
	if (!out)
		ERROR("malloc");
	memset(out, 0, sizeof(*out));
	INIT_LIST_HEAD(&out->sections);
	INIT_LIST_HEAD(&out->symbols);
	INIT_LIST_HEAD(&out->strings);

	/* migrate included sections from kelf to out */
	list_for_each_entry_safe(sec, safesec, &kelf->sections, list) {
		if (!sec->include)
			continue;
		list_del(&sec->list);
		list_add_tail(&sec->list, &out->sections);
		sec->index = 0;
		if (!is_rela_section(sec) && sec->secsym && !sec->secsym->include)
			/* break link to non-included section symbol */
			sec->secsym = NULL;
	}

	/* migrate included symbols from kelf to out */
	list_for_each_entry_safe(sym, safesym, &kelf->symbols, list) {
		if (!sym->include)
			continue;
		list_del(&sym->list);
		list_add_tail(&sym->list, &out->symbols);
		sym->index = 0;
		sym->strip = 0;
		if (sym->sec && !sym->sec->include)
			/* break link to non-included section */
			sym->sec = NULL;
			
	}

	*kelfout = out;
}

void kpatch_reorder_symbols(struct kpatch_elf *kelf)
{
	LIST_HEAD(symbols);

	/* migrate NULL sym */
	kpatch_migrate_symbols(&kelf->symbols, &symbols, is_null_sym);
	/* migrate LOCAL FILE sym */
	kpatch_migrate_symbols(&kelf->symbols, &symbols, is_file_sym);
	/* migrate LOCAL FUNC syms */
	kpatch_migrate_symbols(&kelf->symbols, &symbols, is_local_func_sym);
	/* migrate all other LOCAL syms */
	kpatch_migrate_symbols(&kelf->symbols, &symbols, is_local_sym);
	/* migrate all other (GLOBAL) syms */
	kpatch_migrate_symbols(&kelf->symbols, &symbols, NULL);

	list_replace(&symbols, &kelf->symbols);
}

void kpatch_reindex_elements(struct kpatch_elf *kelf)
{
	struct section *sec;
	struct symbol *sym;
	int index;

	index = 1; /* elf write function handles NULL section 0 */
	list_for_each_entry(sec, &kelf->sections, list)
		sec->index = index++;

	index = 0;
	list_for_each_entry(sym, &kelf->symbols, list) {
		sym->index = index++;
		if (sym->sec)
			sym->sym.st_shndx = sym->sec->index;
		else if (sym->sym.st_shndx != SHN_ABS)
			sym->sym.st_shndx = SHN_UNDEF;
	}
}

int bug_table_group_size(struct kpatch_elf *kelf, int offset)
{
	static int size = 0;
	char *str;

	if (!size) {
		str = getenv("BUG_STRUCT_SIZE");
		if (!str)
			ERROR("BUG_STRUCT_SIZE not set");
		size = atoi(str);
	}

	return size;
}

int parainstructions_group_size(struct kpatch_elf *kelf, int offset)
{
	static int size = 0;
	char *str;

	if (!size) {
		str = getenv("PARA_STRUCT_SIZE");
		if (!str)
			ERROR("PARA_STRUCT_SIZE not set");
		size = atoi(str);
	}

	return size;
}

int ex_table_group_size(struct kpatch_elf *kelf, int offset)
{
	static int size = 0;
	char *str;

	if (!size) {
		str = getenv("EX_STRUCT_SIZE");
		if (!str)
			ERROR("EX_STRUCT_SIZE not set");
		size = atoi(str);
	}

	return size;
}

int altinstructions_group_size(struct kpatch_elf *kelf, int offset)
{
	static int size = 0;
	char *str;

	if (!size) {
		str = getenv("ALT_STRUCT_SIZE");
		if (!str)
			ERROR("ALT_STRUCT_SIZE not set");
		size = atoi(str);
	}

	return size;
}

int smp_locks_group_size(struct kpatch_elf *kelf, int offset)
{
	return 4;
}

/*
 * The rela groups in the .fixup section vary in size.  The beginning of each
 * .fixup rela group is referenced by the __ex_table section. To find the size
 * of a .fixup rela group, we have to traverse the __ex_table relas.
 */
int fixup_group_size(struct kpatch_elf *kelf, int offset)
{
	struct section *sec;
	struct rela *rela;
	int found;

	sec = find_section_by_name(&kelf->sections, ".rela__ex_table");
	if (!sec)
		ERROR("missing .rela__ex_table section");

	/* find beginning of this group */
	found = 0;
	list_for_each_entry(rela, &sec->relas, list) {
		if (!strcmp(rela->sym->name, ".fixup") &&
		    rela->addend == offset) {
				found = 1;
				break;
		}
	}

	if (!found)
		ERROR("can't find .fixup rela group at offset %d\n", offset);

	/* find beginning of next group */
	found = 0;
	list_for_each_entry_continue(rela, &sec->relas, list) {
		if (!strcmp(rela->sym->name, ".fixup") &&
		    rela->addend > offset) {
			found = 1;
			break;
		}
	}

	if (!found) {
		/* last group */
		struct section *fixupsec;
		fixupsec = find_section_by_name(&kelf->sections, ".fixup");
		return fixupsec->sh.sh_size - offset;
	}

	return rela->addend - offset;
}

struct special_section special_sections[] = {
	{
		.name		= "__bug_table",
		.group_size	= bug_table_group_size,
	},
	{
		.name		= ".smp_locks",
		.group_size	= smp_locks_group_size,
	},
	{
		.name		= ".parainstructions",
		.group_size	= parainstructions_group_size,
	},
	{
		.name		= ".fixup",
		.group_size	= fixup_group_size,
	},
	{
		.name		= "__ex_table", /* must come after .fixup */
		.group_size	= ex_table_group_size,
	},
	{
		.name		= ".altinstructions",
		.group_size	= altinstructions_group_size,
	},
	{},
};

int should_keep_rela_group(struct section *sec, int start, int size)
{
	struct rela *rela;
	int found = 0;

	/* check if any relas in the group reference any changed functions */
	list_for_each_entry(rela, &sec->relas, list) {
		if (rela->offset >= start &&
		    rela->offset < start + size &&
		    rela->sym->type == STT_FUNC &&
		    rela->sym->sec->include) {
			found = 1;
			log_debug("new/changed symbol %s found in special section %s\n",
				  rela->sym->name, sec->name);
		}
	}

	return found;
}

/*
 * When updating .fixup, the corresponding addends in .ex_table need to be
 * updated too. Stash the result in rela.r_addend so that the calculation in
 * fixup_group_size() is not affected.
 */
void kpatch_update_ex_table_addend(struct kpatch_elf *kelf,
				   struct special_section *special,
				   int src_offset, int dest_offset,
				   int group_size)
{
	struct rela *rela;
	struct section *sec;

	sec = find_section_by_name(&kelf->sections, ".rela__ex_table");
	if (!sec)
		ERROR("missing .rela__ex_table section");

	list_for_each_entry(rela, &sec->relas, list) {
		if (!strcmp(rela->sym->name, ".fixup") &&
		    rela->addend >= src_offset &&
		    rela->addend < src_offset + group_size)
			rela->rela.r_addend = rela->addend - (src_offset - dest_offset);
	}
}

void kpatch_regenerate_special_section(struct kpatch_elf *kelf,
				       struct special_section *special,
				       struct section *sec)
{
	struct rela *rela, *safe;
	char *src, *dest;
	int group_size, src_offset, dest_offset, include, align, aligned_size;

	LIST_HEAD(newrelas);

	src = sec->base->data->d_buf;
	/* alloc buffer for new base section */
	dest = malloc(sec->base->sh.sh_size);
	if (!dest)
		ERROR("malloc");

	/* Restore the stashed r_addend from kpatch_update_ex_table_addend. */
	if (!strcmp(special->name, "__ex_table")) {
		list_for_each_entry(rela, &sec->relas, list) {
			if (!strcmp(rela->sym->name, ".fixup"))
				rela->addend = rela->rela.r_addend;
		}
	}

	group_size = 0;
	src_offset = 0;
	dest_offset = 0;
	for ( ; src_offset < sec->base->sh.sh_size; src_offset += group_size) {

		group_size = special->group_size(kelf, src_offset);
		include = should_keep_rela_group(sec, src_offset, group_size);

		if (!include)
			continue;

		/*
		 * Copy all relas in the group.  It's possible that the relas
		 * aren't sorted (e.g. .rela.fixup), so go through the entire
		 * rela list each time.
		 */
		list_for_each_entry_safe(rela, safe, &sec->relas, list) {
			if (rela->offset >= src_offset &&
			    rela->offset < src_offset + group_size) {
				/* copy rela entry */
				list_del(&rela->list);
				list_add_tail(&rela->list, &newrelas);

				rela->offset -= src_offset - dest_offset;
				rela->rela.r_offset = rela->offset;

				rela->sym->include = 1;


				if (!strcmp(special->name, ".fixup"))
					kpatch_update_ex_table_addend(kelf, special,
								      src_offset,
								      dest_offset,
								      group_size);
			}
		}

		/* copy base section group */
		memcpy(dest + dest_offset, src + src_offset, group_size);
		dest_offset += group_size;
	}

	/* verify that group_size is a divisor of aligned section size */
	align = sec->base->sh.sh_addralign;
	aligned_size = ((sec->base->sh.sh_size + align - 1) / align) * align;
	if (src_offset != aligned_size)
		ERROR("group size mismatch for section %s\n", sec->base->name);

	if (!dest_offset) {
		/* no changed or global functions referenced */
		sec->status = sec->base->status = SAME;
		sec->include = sec->base->include = 0;
		free(dest);
		return;
	}

	/* overwrite with new relas list */
	list_replace(&newrelas, &sec->relas);

	/* include both rela and base sections */
	sec->include = 1;
	sec->base->include = 1;

	/*
	 * Update text section data buf and size.
	 *
	 * The rela section's data buf and size will be regenerated in
	 * kpatch_rebuild_rela_section_data().
	 */
	sec->base->data->d_buf = dest;
	sec->base->data->d_size = dest_offset;
}

void kpatch_include_debug_sections(struct kpatch_elf *kelf)
{
	struct section *sec;
	struct rela *rela, *saferela;

	/* include all .debug_* sections */
	list_for_each_entry(sec, &kelf->sections, list) {
		if (is_debug_section(sec)) {
			sec->include = 1;
			if (!is_rela_section(sec))
				sec->secsym->include = 1;
		}
	}

	/*
	 * Go through the .rela.debug_ sections and strip entries
	 * referencing unchanged symbols
	 */
	list_for_each_entry(sec, &kelf->sections, list) {
		if (!is_rela_section(sec) || !is_debug_section(sec))
			continue;
		list_for_each_entry_safe(rela, saferela, &sec->relas, list)
			if (!rela->sym->sec->include)
				list_del(&rela->list);
	}
}

void kpatch_mark_ignored_sections(struct kpatch_elf *kelf)
{
	struct section *sec, *strsec, *ignoresec;
	struct rela *rela;
	char *name;

	sec = find_section_by_name(&kelf->sections, ".kpatch.ignore.sections");
	if (!sec)
		return;

	list_for_each_entry(rela, &sec->rela->relas, list) {
		strsec = rela->sym->sec;
		strsec->status = CHANGED;
		/*
		 * Include the string section here.  This is because the
		 * KPATCH_IGNORE_SECTION() macro is passed a literal string
		 * by the patch author, resulting in a change to the string
		 * section.  If we don't include it, then we will potentially
		 * get a "changed section not included" error in
		 * kpatch_verify_patchability() if no other function based change
		 * also changes the string section.  We could try to exclude each
		 * literal string added to the section by KPATCH_IGNORE_SECTION()
		 * from the section data comparison, but this is a simpler way.
		 */
		strsec->include = 1;
		name = strsec->data->d_buf + rela->addend;
		ignoresec = find_section_by_name(&kelf->sections, name);
		if (!ignoresec)
			ERROR("KPATCH_IGNORE_SECTION: can't find %s", name);
		log_normal("ignoring section: %s\n", name);
		if (is_rela_section(ignoresec))
			ignoresec = ignoresec->base;
		ignoresec->ignore = 1;
		if (ignoresec->twin)
			ignoresec->twin->ignore = 1;
	}
}

void kpatch_mark_ignored_sections_same(struct kpatch_elf *kelf)
{
	struct section *sec;
	struct symbol *sym;

	list_for_each_entry(sec, &kelf->sections, list) {
		if (!sec->ignore)
			continue;
		sec->status = SAME;
		if (sec->secsym)
			sec->secsym->status = SAME;
		if (sec->rela)
			sec->rela->status = SAME;
		list_for_each_entry(sym, &kelf->symbols, list) {
			if (sym->sec != sec)
				continue;
			sym->status = SAME;
		}
	}
}

void kpatch_mark_ignored_functions_same(struct kpatch_elf *kelf)
{
	struct section *sec;
	struct rela *rela;

	sec = find_section_by_name(&kelf->sections, ".kpatch.ignore.functions");
	if (!sec)
		return;

	list_for_each_entry(rela, &sec->rela->relas, list) {
		if (!rela->sym->sec)
			ERROR("expected bundled symbol");
		if (rela->sym->type != STT_FUNC)
			ERROR("expected function symbol");
		log_normal("ignoring function: %s\n", rela->sym->name);
		if (rela->sym->status != CHANGED)
			log_normal("NOTICE: no change detected in function %s, unnecessary KPATCH_IGNORE_FUNCTION()?\n", rela->sym->name);
		rela->sym->status = SAME;
		rela->sym->sec->status = SAME;
		if (rela->sym->sec->secsym)
			rela->sym->sec->secsym->status = SAME;
		if (rela->sym->sec->rela)
			rela->sym->sec->rela->status = SAME;
	}
}

void kpatch_process_special_sections(struct kpatch_elf *kelf)
{
	struct special_section *special;
	struct section *sec;
	struct symbol *sym;
	struct rela *rela;
	int altinstr = 0;

	for (special = special_sections; special->name; special++) {
		sec = find_section_by_name(&kelf->sections, special->name);
		if (!sec)
			continue;

		sec = sec->rela;
		if (!sec)
			continue;

		kpatch_regenerate_special_section(kelf, special, sec);

		if (!strcmp(special->name, ".altinstructions") && sec->base->include)
			altinstr = 1;
	}

	/*
	 * The following special sections don't have relas which reference
	 * non-included symbols, so their entire rela section can be included.
	 */
	list_for_each_entry(sec, &kelf->sections, list) {
		if (strcmp(sec->name, ".altinstr_replacement"))
			continue;
		/*
		 * Only include .altinstr_replacement if .altinstructions
		 * is also included.
		 */
		if (!altinstr)
			break;

		/* include base section */
		sec->include = 1;

		/* include all symbols in the section */
		list_for_each_entry(sym, &kelf->symbols, list)
			if (sym->sec == sec)
				sym->include = 1;

		/* include rela section */
		if (sec->rela) {
			sec->rela->include = 1;
			/* include all symbols referenced by relas */
			list_for_each_entry(rela, &sec->rela->relas, list)
				rela->sym->include = 1;
		}
	}

	/*
	 * The following special sections aren't supported, so make sure we
	 * don't ever try to include them.  Otherwise the kernel will see the
	 * jump table during module loading and get confused.  Generally it
	 * should be safe to exclude them, it just means that you can't modify
	 * jump labels and enable tracepoints in a patched function.
	 */
	list_for_each_entry(sec, &kelf->sections, list) {
		if (strcmp(sec->name, "__jump_table") &&
		    strcmp(sec->name, "__tracepoints") &&
		    strcmp(sec->name, "__tracepoints_ptrs") &&
		    strcmp(sec->name, "__tracepoints_strings"))
			continue;

		sec->status = SAME;
		sec->include = 0;
		if (sec->rela) {
			sec->rela->status = SAME;
			sec->rela->include = 0;
		}
	}
}

void kpatch_create_patches_sections(struct kpatch_elf *kelf,
                                    struct lookup_table *table, char *hint,
                                    char *objname)
{
	int nr, index, objname_offset;
	struct section *sec, *relasec;
	struct symbol *sym, *strsym;
	struct rela *rela;
	struct lookup_result result;
	struct kpatch_patch_func *funcs;

	/* count patched functions */
	nr = 0;
	list_for_each_entry(sym, &kelf->symbols, list)
		if (sym->type == STT_FUNC && sym->status == CHANGED)
			nr++;

	/* create text/rela section pair */
	sec = create_section_pair(kelf, ".kpatch.funcs", sizeof(*funcs), nr);
	relasec = sec->rela;
	funcs = sec->data->d_buf;

	/* lookup strings symbol */
	strsym = find_symbol_by_name(&kelf->symbols, ".kpatch.strings");
	if (!strsym)
		ERROR("can't find .kpatch.strings symbol");

	/* add objname to strings */
	objname_offset = offset_of_string(&kelf->strings, objname);

	/* populate sections */
	index = 0;
	list_for_each_entry(sym, &kelf->symbols, list) {
		if (sym->type == STT_FUNC && sym->status == CHANGED) {
			if (sym->bind == STB_LOCAL) {
				if (lookup_local_symbol(table, sym->name,
				                        hint, &result))
					ERROR("lookup_local_symbol %s (%s)",
					      sym->name, hint);
			} else {
				if(lookup_global_symbol(table, sym->name,
				                        &result))
					ERROR("lookup_global_symbol %s",
					      sym->name);
			}
			log_debug("lookup for %s @ 0x%016lx len %lu\n",
			          sym->name, result.value, result.size);

			/* add entry in text section */
			funcs[index].old_addr = result.value;
			funcs[index].old_size = result.size;
			funcs[index].new_size = sym->sym.st_size;
			funcs[index].sympos = result.pos;

			/*
			 * Add a relocation that will populate
			 * the funcs[index].new_addr field at
			 * module load time.
			 */
			ALLOC_LINK(rela, &relasec->relas);
			rela->sym = sym;
			rela->type = R_X86_64_64;
			rela->addend = 0;
			rela->offset = index * sizeof(*funcs);

			/*
			 * Add a relocation that will populate
			 * the funcs[index].name field.
			 */
			ALLOC_LINK(rela, &relasec->relas);
			rela->sym = strsym;
			rela->type = R_X86_64_64;
			rela->addend = offset_of_string(&kelf->strings, sym->name);
			rela->offset = index * sizeof(*funcs) +
			               offsetof(struct kpatch_patch_func, name);

			/*
			 * Add a relocation that will populate
			 * the funcs[index].objname field.
			 */
			ALLOC_LINK(rela, &relasec->relas);
			rela->sym = strsym;
			rela->type = R_X86_64_64;
			rela->addend = objname_offset;
			rela->offset = index * sizeof(*funcs) +
			               offsetof(struct kpatch_patch_func,objname);

			index++;
		}
	}

	/* sanity check, index should equal nr */
	if (index != nr)
		ERROR("size mismatch in funcs sections");

}

static int kpatch_is_core_module_symbol(char *name)
{
	return (!strcmp(name, "kpatch_shadow_alloc") ||
		!strcmp(name, "kpatch_shadow_free") ||
		!strcmp(name, "kpatch_shadow_get"));
}

void kpatch_create_dynamic_rela_sections(struct kpatch_elf *kelf,
                                         struct lookup_table *table, char *hint,
                                         char *objname)
{
	int nr, index, objname_offset;
	struct section *sec, *dynsec, *relasec;
	struct rela *rela, *dynrela, *safe;
	struct symbol *strsym;
	struct lookup_result result;
	struct kpatch_patch_dynrela *dynrelas;
	int vmlinux, external, ret;

	vmlinux = !strcmp(objname, "vmlinux");

	/* count rela entries that need to be dynamic */
	nr = 0;
	list_for_each_entry(sec, &kelf->sections, list) {
		if (!is_rela_section(sec))
			continue;
		if (!strcmp(sec->name, ".rela.kpatch.funcs"))
			continue;
		list_for_each_entry(rela, &sec->relas, list)
			nr++; /* upper bound on number of dynrelas */
	}

	/* create text/rela section pair */
	dynsec = create_section_pair(kelf, ".kpatch.dynrelas", sizeof(*dynrelas), nr);
	relasec = dynsec->rela;
	dynrelas = dynsec->data->d_buf;

	/* lookup strings symbol */
	strsym = find_symbol_by_name(&kelf->symbols, ".kpatch.strings");
	if (!strsym)
		ERROR("can't find .kpatch.strings symbol");

	/* add objname to strings */
	objname_offset = offset_of_string(&kelf->strings, objname);

	/* populate sections */
	index = 0;
	list_for_each_entry(sec, &kelf->sections, list) {
		if (!is_rela_section(sec))
			continue;
		if (!strcmp(sec->name, ".rela.kpatch.funcs") ||
		    !strcmp(sec->name, ".rela.kpatch.dynrelas"))
			continue;
		list_for_each_entry_safe(rela, safe, &sec->relas, list) {
			if (rela->sym->sec)
				continue;
			/*
			 * Allow references to core module symbols to remain as
			 * normal relas, since the core module may not be
			 * compiled into the kernel, and they should be
			 * exported anyway.
			 */
			if (kpatch_is_core_module_symbol(rela->sym->name))
				continue;

			external = 0;

			if (rela->sym->bind == STB_LOCAL) {
				/* An unchanged local symbol */
				ret = lookup_local_symbol(table,
					rela->sym->name, hint, &result);
				if (ret == 2)
					ERROR("lookup_local_symbol: ambiguous %s:%s relocation, needed for %s",
			               hint, rela->sym->name, sec->base->name);
				else if (ret)
					ERROR("lookup_local_symbol %s:%s needed for %s",
			               hint, rela->sym->name, sec->base->name);

			}
			else if (vmlinux) {
				/*
				 * We have a patch to vmlinux which references
				 * a global symbol.  Use a normal rela for
				 * exported symbols and a dynrela otherwise.
				 */
				if (lookup_is_exported_symbol(table, rela->sym->name))
					continue;

				/*
				 * If lookup_global_symbol() fails, assume the
				 * symbol is defined in another object in the
				 * patch module.
				 */
				if (lookup_global_symbol(table, rela->sym->name,
							 &result))
					continue;
			} else {
				/*
				 * We have a patch to a module which references
				 * a global symbol.
				 */

				/*
				 * __fentry__ relas can't be converted to
				 * dynrelas because the ftrace module init code
				 * runs before the dynrela code can initialize
				 * them.  __fentry__ is exported by the kernel,
				 * so leave it as a normal rela.
				 */
				if (!strcmp(rela->sym->name, "__fentry__"))
					continue;

				/*
				 * Try to find the symbol in the module being
				 * patched.
				 */
				if (lookup_global_symbol(table, rela->sym->name,
							 &result))
					/*
					 * Not there, assume it's either an
					 * exported symbol or provided by
					 * another .o in the patch module.
					 */
					external = 1;
			}
			log_debug("lookup for %s @ 0x%016lx len %lu\n",
			          rela->sym->name, result.value, result.size);

			/* dest filed in by rela entry below */
			if (vmlinux)
				dynrelas[index].src = result.value;
			else
				/* for modules, src is discovered at runtime */
				dynrelas[index].src = 0;
			dynrelas[index].addend = rela->addend;
			dynrelas[index].type = rela->type;
			dynrelas[index].external = external;
			dynrelas[index].sympos = result.pos;

			/* add rela to fill in dest field */
			ALLOC_LINK(dynrela, &relasec->relas);
			if (sec->base->sym)
				dynrela->sym = sec->base->sym;
			else
				dynrela->sym = sec->base->secsym;
			dynrela->type = R_X86_64_64;
			dynrela->addend = rela->offset;
			dynrela->offset = index * sizeof(*dynrelas);

			/* add rela to fill in name field */
			ALLOC_LINK(dynrela, &relasec->relas);
			dynrela->sym = strsym;
			dynrela->type = R_X86_64_64;
			dynrela->addend = offset_of_string(&kelf->strings, rela->sym->name);
			dynrela->offset = index * sizeof(*dynrelas) + offsetof(struct kpatch_patch_dynrela, name);

			/* add rela to fill in objname field */
			ALLOC_LINK(dynrela, &relasec->relas);
			dynrela->sym = strsym;
			dynrela->type = R_X86_64_64;
			dynrela->addend = objname_offset;
			dynrela->offset = index * sizeof(*dynrelas) +
				offsetof(struct kpatch_patch_dynrela, objname);

			rela->sym->strip = 1;
			list_del(&rela->list);
			free(rela);

			index++;
		}
	}

	/* set size to actual number of dynrelas */
	dynsec->data->d_size = index * sizeof(struct kpatch_patch_dynrela);
	dynsec->sh.sh_size = dynsec->data->d_size;
}

void kpatch_create_hooks_objname_rela(struct kpatch_elf *kelf, char *objname)
{
	struct section *sec;
	struct rela *rela;
	struct symbol *strsym;
	int objname_offset;

	/* lookup strings symbol */
	strsym = find_symbol_by_name(&kelf->symbols, ".kpatch.strings");
	if (!strsym)
		ERROR("can't find .kpatch.strings symbol");

	/* add objname to strings */
	objname_offset = offset_of_string(&kelf->strings, objname);

	list_for_each_entry(sec, &kelf->sections, list) {
		if (strcmp(sec->name, ".rela.kpatch.hooks.load") &&
		    strcmp(sec->name, ".rela.kpatch.hooks.unload"))
			continue;

		ALLOC_LINK(rela, &sec->relas);
		rela->sym = strsym;
		rela->type = R_X86_64_64;
		rela->addend = objname_offset;
		rela->offset = offsetof(struct kpatch_patch_hook, objname);
	}
}

/*
 * This function basically reimplements the functionality of the Linux
 * recordmcount script, so that patched functions can be recognized by ftrace.
 *
 * TODO: Eventually we can modify recordmount so that it recognizes our bundled
 * sections as valid and does this work for us.
 */
void kpatch_create_mcount_sections(struct kpatch_elf *kelf)
{
	int nr, index;
	struct section *sec, *relasec;
	struct symbol *sym;
	struct rela *rela;
	void **funcs, *newdata;
	unsigned char *insn;

	nr = 0;
	list_for_each_entry(sym, &kelf->symbols, list)
		if (sym->type == STT_FUNC && sym->status != SAME &&
		    sym->has_fentry_call)
			nr++;

	/* create text/rela section pair */
	sec = create_section_pair(kelf, "__mcount_loc", sizeof(*funcs), nr);
	relasec = sec->rela;
	funcs = sec->data->d_buf;

	/* populate sections */
	index = 0;
	list_for_each_entry(sym, &kelf->symbols, list) {
		if (sym->type != STT_FUNC || sym->status == SAME)
			continue;

		if (!sym->has_fentry_call) {
			log_debug("function %s has no fentry call, no mcount record is needed\n",
				  sym->name);
			continue;
		}

		/* add rela in .rela__mcount_loc to fill in function pointer */
		ALLOC_LINK(rela, &relasec->relas);
		rela->sym = sym;
		rela->type = R_X86_64_64;
		rela->addend = 0;
		rela->offset = index * sizeof(*funcs);

		/*
		 * Modify the first instruction of the function to "callq
		 * __fentry__" so that ftrace will be happy.
		 */
		newdata = malloc(sym->sec->data->d_size);
		memcpy(newdata, sym->sec->data->d_buf, sym->sec->data->d_size);
		sym->sec->data->d_buf = newdata;
		insn = newdata;
		if (insn[0] != 0xf)
			ERROR("%s: unexpected instruction at the start of the function",
			      sym->name);
		insn[0] = 0xe8;
		insn[1] = 0;
		insn[2] = 0;
		insn[3] = 0;
		insn[4] = 0;

		rela = list_first_entry(&sym->sec->rela->relas, struct rela,
					list);
		rela->type = R_X86_64_PC32;

		index++;
	}

	/* sanity check, index should equal nr */
	if (index != nr)
		ERROR("size mismatch in funcs sections");
}

/*
 * This function strips out symbols that were referenced by changed rela
 * sections, but the rela entries that referenced them were converted to
 * dynrelas and are no longer needed.
 */
void kpatch_strip_unneeded_syms(struct kpatch_elf *kelf,
                                struct lookup_table *table)
{
	struct symbol *sym, *safe;

	list_for_each_entry_safe(sym, safe, &kelf->symbols, list) {
		if (sym->strip) {
			list_del(&sym->list);
			free(sym);
		}
	}
}

void kpatch_create_strings_elements(struct kpatch_elf *kelf)
{
	struct section *sec;
	struct symbol *sym;

	/* create .kpatch.strings */

	/* allocate section resources */
	ALLOC_LINK(sec, &kelf->sections);
	sec->name = ".kpatch.strings";

	/* set data */
	sec->data = malloc(sizeof(*sec->data));
	if (!sec->data)
		ERROR("malloc");
	sec->data->d_type = ELF_T_BYTE;

	/* set section header */
	sec->sh.sh_type = SHT_PROGBITS;
	sec->sh.sh_entsize = 1;
	sec->sh.sh_addralign = 1;
	sec->sh.sh_flags = SHF_ALLOC;

	/* create .kpatch.strings section symbol (reuse sym variable) */

	ALLOC_LINK(sym, &kelf->symbols);
	sym->sec = sec;
	sym->sym.st_info = GELF_ST_INFO(STB_LOCAL, STT_SECTION);
	sym->type = STT_SECTION;
	sym->bind = STB_LOCAL;
	sym->name = ".kpatch.strings";
}

void kpatch_build_strings_section_data(struct kpatch_elf *kelf)
{
	struct string *string;
	struct section *sec;
	int size;
	char *strtab;

	sec = find_section_by_name(&kelf->sections, ".kpatch.strings");
	if (!sec)
		ERROR("can't find .kpatch.strings");

	/* determine size */
	size = 0;
	list_for_each_entry(string, &kelf->strings, list)
		size += strlen(string->name) + 1;

	/* allocate section resources */
	strtab = malloc(size);
	if (!strtab)
		ERROR("malloc");
	sec->data->d_buf = strtab;
	sec->data->d_size = size;

	/* populate strings section data */
	list_for_each_entry(string, &kelf->strings, list) {
		strcpy(strtab, string->name);
		strtab += strlen(string->name) + 1;
	}
}

void kpatch_rebuild_rela_section_data(struct section *sec)
{
	struct rela *rela;
	int nr = 0, index = 0, size;
	GElf_Rela *relas;

	list_for_each_entry(rela, &sec->relas, list)
		nr++;

	size = nr * sizeof(*relas);
	relas = malloc(size);
	if (!relas)
		ERROR("malloc");

	sec->data->d_buf = relas;
	sec->data->d_size = size;
	/* d_type remains ELF_T_RELA */

	sec->sh.sh_size = size;

	list_for_each_entry(rela, &sec->relas, list) {
		relas[index].r_offset = rela->offset;
		relas[index].r_addend = rela->addend;
		relas[index].r_info = GELF_R_INFO(rela->sym->index, rela->type);
		index++;
	}

	/* sanity check, index should equal nr */
	if (index != nr)
		ERROR("size mismatch in rebuilt rela section");
}

struct arguments {
	char *args[4];
	int debug;
};

static char args_doc[] = "original.o patched.o kernel-object output.o";

static struct argp_option options[] = {
	{"debug", 'd', 0, 0, "Show debug output" },
	{ 0 }
};

static error_t parse_opt (int key, char *arg, struct argp_state *state)
{
	/* Get the input argument from argp_parse, which we
	   know is a pointer to our arguments structure. */
	struct arguments *arguments = state->input;

	switch (key)
	{
		case 'd':
			arguments->debug = 1;
			break;
		case ARGP_KEY_ARG:
			if (state->arg_num >= 4)
				/* Too many arguments. */
				argp_usage (state);
			arguments->args[state->arg_num] = arg;
			break;
		case ARGP_KEY_END:
			if (state->arg_num < 4)
				/* Not enough arguments. */
				argp_usage (state);
			break;
		default:
			return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static struct argp argp = { options, parse_opt, args_doc, 0 };

int main(int argc, char *argv[])
{
	struct kpatch_elf *kelf_base, *kelf_patched, *kelf_out;
	struct arguments arguments;
	int num_changed, hooks_exist, new_globals_exist;
	struct lookup_table *lookup;
	struct section *sec, *symtab;
	struct symbol *sym;
	char *hint = NULL, *name, *pos;

	arguments.debug = 0;
	argp_parse (&argp, argc, argv, 0, 0, &arguments);
	if (arguments.debug)
		loglevel = DEBUG;

	elf_version(EV_CURRENT);

	childobj = basename(arguments.args[0]);

	kelf_base = kpatch_elf_open(arguments.args[0]);
	kelf_patched = kpatch_elf_open(arguments.args[1]);

	kpatch_compare_elf_headers(kelf_base->elf, kelf_patched->elf);
	kpatch_check_program_headers(kelf_base->elf);
	kpatch_check_program_headers(kelf_patched->elf);

	kpatch_mark_grouped_sections(kelf_patched);
	kpatch_replace_sections_syms(kelf_base);
	kpatch_replace_sections_syms(kelf_patched);
	kpatch_rename_mangled_functions(kelf_base, kelf_patched);

	kpatch_correlate_elfs(kelf_base, kelf_patched);
	kpatch_correlate_static_local_variables(kelf_base, kelf_patched);

	/*
	 * After this point, we don't care about kelf_base anymore.
	 * We access its sections via the twin pointers in the
	 * section, symbol, and rela lists of kelf_patched.
	 */
	kpatch_mark_ignored_sections(kelf_patched);
	kpatch_compare_correlated_elements(kelf_patched);
	kpatch_check_fentry_calls(kelf_patched);
	kpatch_elf_teardown(kelf_base);
	kpatch_elf_free(kelf_base);

	kpatch_mark_ignored_functions_same(kelf_patched);
	kpatch_mark_ignored_sections_same(kelf_patched);

	kpatch_include_standard_elements(kelf_patched);
	num_changed = kpatch_include_changed_functions(kelf_patched);
	kpatch_include_debug_sections(kelf_patched);
	hooks_exist = kpatch_include_hook_elements(kelf_patched);
	kpatch_include_force_elements(kelf_patched);
	new_globals_exist = kpatch_include_new_globals(kelf_patched);

	kpatch_print_changes(kelf_patched);
	kpatch_dump_kelf(kelf_patched);

	kpatch_process_special_sections(kelf_patched);
	kpatch_verify_patchability(kelf_patched);

	if (!num_changed && !new_globals_exist) {
		if (hooks_exist)
			log_debug("no changed functions were found, but hooks exist\n");
		else {
			log_debug("no changed functions were found\n");
			return 3; /* 1 is ERROR, 2 is DIFF_FATAL */
		}
	}

	/* this is destructive to kelf_patched */
	kpatch_migrate_included_elements(kelf_patched, &kelf_out);

	/*
	 * Teardown kelf_patched since we shouldn't access sections or symbols
	 * through it anymore.  Don't free however, since our section and symbol
	 * name fields still point to strings in the Elf object owned by
	 * kpatch_patched.
	 */
	kpatch_elf_teardown(kelf_patched);

	list_for_each_entry(sym, &kelf_out->symbols, list) {
		if (sym->type == STT_FILE) {
			hint = sym->name;
			break;
		}
	}
	if (!hint)
		ERROR("FILE symbol not found in output. Stripped?\n");

	/* create symbol lookup table */
	lookup = lookup_open(arguments.args[2]);

	/* extract module name (destructive to arguments.modulefile) */
	name = basename(arguments.args[2]);
	if (!strncmp(name, "vmlinux-", 8))
		name = "vmlinux";
	else {
		pos = strchr(name,'.');
		if (pos) {
			/* kernel module */
			*pos = '\0';
			pos = name;
			while ((pos = strchr(pos, '-')))
				*pos++ = '_';
		}
	}

	/* create strings, patches, and dynrelas sections */
	kpatch_create_strings_elements(kelf_out);
	kpatch_create_patches_sections(kelf_out, lookup, hint, name);
	kpatch_create_dynamic_rela_sections(kelf_out, lookup, hint, name);
	kpatch_create_hooks_objname_rela(kelf_out, name);
	kpatch_build_strings_section_data(kelf_out);

	kpatch_create_mcount_sections(kelf_out);

	/*
	 *  At this point, the set of output sections and symbols is
	 *  finalized.  Reorder the symbols into linker-compliant
	 *  order and index all the symbols and sections.  After the
	 *  indexes have been established, update index data
	 *  throughout the structure.
	 */
	kpatch_reorder_symbols(kelf_out);
	kpatch_strip_unneeded_syms(kelf_out, lookup);
	kpatch_reindex_elements(kelf_out);

	/*
	 * Update rela section headers and rebuild the rela section data
	 * buffers from the relas lists.
	 */
	symtab = find_section_by_name(&kelf_out->sections, ".symtab");
	list_for_each_entry(sec, &kelf_out->sections, list) {
		if (!is_rela_section(sec))
			continue;
		sec->sh.sh_link = symtab->index;
		sec->sh.sh_info = sec->base->index;
		kpatch_rebuild_rela_section_data(sec);
	}

	kpatch_create_shstrtab(kelf_out);
	kpatch_create_strtab(kelf_out);
	kpatch_create_symtab(kelf_out);
	kpatch_dump_kelf(kelf_out);
	kpatch_write_output_elf(kelf_out, kelf_patched->elf, arguments.args[3]);

	kpatch_elf_free(kelf_patched);
	kpatch_elf_teardown(kelf_out);
	kpatch_elf_free(kelf_out);

	return 0;
}
