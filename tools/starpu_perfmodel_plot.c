/* StarPU --- Runtime system for heterogeneous multicore architectures.
 *
 * Copyright (C) 2011-2014  Université de Bordeaux
 * Copyright (C) 2011, 2012, 2013, 2014  Centre National de la Recherche Scientifique
 * Copyright (C) 2011  Télécom-SudParis
 *
 * StarPU is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 *
 * StarPU is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU Lesser General Public License in COPYING.LGPL for more details.
 */

#include <config.h>
#include <assert.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/stat.h>
#include <limits.h>
#ifdef STARPU_USE_FXT
#include <common/fxt.h>
#endif
#include <common/utils.h>

#include <starpu.h>
#include <core/perfmodel/perfmodel.h> // we need to browse the list associated to history-based models
#include <core/workers.h>

#if defined(_WIN32) && !defined(__CYGWIN__)
#include <windows.h>
#endif

#define PROGNAME "starpu_perfmodel_plot"

struct _perfmodel_plot_options
{
	/* display all available models */
	int list;
	/* what kernel ? */
	char *symbol;
	/* which combination */
	int comb_is_set;
	int comb;
	/* display all available combinations of a specific model */
	int list_combs;
	int gflops;
	/* Unless a FxT file is specified, we just display the model */
	int with_fxt_file;

	char avg_file_name[256];

#ifdef STARPU_USE_FXT
	struct starpu_fxt_codelet_event *dumped_codelets;
	struct starpu_fxt_options fxt_options;
	char data_file_name[256];
#endif
};

static void usage()
{
	fprintf(stderr, "Draw a graph corresponding to the execution time of a given perfmodel\n");
	fprintf(stderr, "Usage: %s [ options ]\n", PROGNAME);
        fprintf(stderr, "\n");
	fprintf(stderr, "One must specify a symbol with the -s option or use -l\n");
        fprintf(stderr, "Options:\n");
        fprintf(stderr, "   -l                  display all available models\n");
        fprintf(stderr, "   -s <symbol>         specify the symbol\n");
	fprintf(stderr, "   -f                  draw GFlops instead of time\n");
	fprintf(stderr, "   -i <Fxt files>      input FxT files generated by StarPU\n");
	fprintf(stderr, "   -lc                 display all combinations of a given model\n");
        fprintf(stderr, "   -c <combination>    specify the combination (use the option -lc to list all combinations of a given model)\n");
	fprintf(stderr, "   -h, --help          display this help and exit\n");
	fprintf(stderr, "   -v, --version       output version information and exit\n\n");
        fprintf(stderr, "Report bugs to <%s>.", PACKAGE_BUGREPORT);
        fprintf(stderr, "\n");
}

static void parse_args(int argc, char **argv, struct _perfmodel_plot_options *options)
{
	memset(options, 0, sizeof(struct _perfmodel_plot_options));

#ifdef STARPU_USE_FXT
	/* Default options */
	starpu_fxt_options_init(&options->fxt_options);

	options->fxt_options.out_paje_path = NULL;
	options->fxt_options.activity_path = NULL;
	options->fxt_options.distrib_time_path = NULL;
	options->fxt_options.dag_path = NULL;

	options->fxt_options.dumped_codelets = &options->dumped_codelets;
#endif

	/* We want to support arguments such as "-i trace_*" */
	unsigned reading_input_filenames = 0;

	int i;
	for (i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "-s") == 0)
		{
			options->symbol = argv[++i];
			continue;
		}

		if (strcmp(argv[i], "-i") == 0)
		{
			reading_input_filenames = 1;
#ifdef STARPU_USE_FXT
			options->fxt_options.filenames[options->fxt_options.ninputfiles++] = argv[++i];
			options->with_fxt_file = 1;
#else
			fprintf(stderr, "Warning: FxT support was not enabled in StarPU: FxT traces will thus be ignored!\n");
#endif
			continue;
		}

		if (strcmp(argv[i], "-l") == 0)
		{
			options->list = 1;
			continue;
		}

		if (strcmp(argv[i], "-lc") == 0)
		{
			options->list_combs = 1;
			continue;
		}

		if (strcmp(argv[i], "-f") == 0)
		{
			options->gflops = 1;
			continue;
		}

		if (strcmp(argv[i], "-c") == 0)
		{
			options->comb_is_set = 1;
			options->comb = atoi(argv[++i]);
			continue;
		}

		if (strcmp(argv[i], "-h") == 0 ||
		    strcmp(argv[i], "--help") == 0)
		{
			usage();
			exit(EXIT_SUCCESS);
		}

		if (strcmp(argv[i], "-v") == 0 ||
		    strcmp(argv[i], "--version") == 0)
                {
		        fputs(PROGNAME " (" PACKAGE_NAME ") " PACKAGE_VERSION "\n", stderr);
			exit(EXIT_SUCCESS);
		}

		/* If the reading_input_filenames flag is set, and that the
		 * argument does not match an option, we assume this may be
		 * another filename */
		if (reading_input_filenames)
		{
#ifdef STARPU_USE_FXT
			options->fxt_options.filenames[options->fxt_options.ninputfiles++] = argv[i];
#endif
			continue;
		}
	}

	if ((!options->symbol && !options->list) || (options->list_combs && !options->symbol))
	{
		fprintf(stderr, "Incorrect usage, aborting\n");
                usage();
		exit(-1);
	}
}

static char *replace_char(char *str, char old, char new)
{
	char *p = strdup(str);
	char *ptr = p;
	while (*ptr)
	{
		if (*ptr == old) *ptr = new;
		ptr ++;
	}
	return p;
}

static void print_comma(FILE *gnuplot_file, int *first)
{
	if (*first)
	{
		*first = 0;
	}
	else
	{
		fprintf(gnuplot_file, ",\\\n\t");
	}
}

static void display_perf_model(FILE *gnuplot_file, struct starpu_perfmodel_arch* arch, struct starpu_perfmodel_per_arch *arch_model, int impl, int *first, struct _perfmodel_plot_options *options)
{
	char arch_name[256];

	starpu_perfmodel_get_arch_name(arch, arch_name, 256, impl);

#ifdef STARPU_USE_FXT
	if (!options->gflops && options->with_fxt_file && impl == 0)
	{
		print_comma(gnuplot_file, first);
		fprintf(gnuplot_file, "\"< grep -w \\^%s %s\" using 2:3 title \"Profiling %s\"", arch_name, options->data_file_name, replace_char(arch_name, '_', '-'));
	}
#endif

	/* Only display the regression model if we could actually build a model */
	if (!options->gflops && arch_model->regression.valid && !arch_model->regression.nl_valid)
	{
		print_comma(gnuplot_file, first);

		fprintf(stderr, "\tLinear: y = alpha size ^ beta\n");
		fprintf(stderr, "\t\talpha = %e\n", arch_model->regression.alpha * 0.001);
		fprintf(stderr, "\t\tbeta = %e\n", arch_model->regression.beta);

		fprintf(gnuplot_file, "0.001 * %f * x ** %f title \"Linear Regression %s\"",
			arch_model->regression.alpha, arch_model->regression.beta, arch_name);
	}

	if (!options->gflops && arch_model->regression.nl_valid)
	{
		print_comma(gnuplot_file, first);

		fprintf(stderr, "\tNon-Linear: y = a size ^b + c\n");
		fprintf(stderr, "\t\ta = %e\n", arch_model->regression.a * 0.001);
		fprintf(stderr, "\t\tb = %e\n", arch_model->regression.b);
		fprintf(stderr, "\t\tc = %e\n", arch_model->regression.c * 0.001);

		fprintf(gnuplot_file, "0.001 * %f * x ** %f + 0.001 * %f title \"Non-Linear Regression %s\"",
			arch_model->regression.a, arch_model->regression.b,  arch_model->regression.c, arch_name);
	}
}

static void display_history_based_perf_models(FILE *gnuplot_file, struct starpu_perfmodel *model, int *first, struct _perfmodel_plot_options *options)
{
	FILE *datafile;
	struct starpu_perfmodel_history_list *ptr;
	char arch_name[32];
	int col;
	unsigned long last, minimum = 0;

	datafile = fopen(options->avg_file_name, "w");
	col = 2;

	int i;
	for(i = 0; i < model->state->ncombs; i++)
	{
		int comb = model->state->combs[i];
		if (options->comb_is_set == 0 || options->comb == comb)
		{
			struct starpu_perfmodel_arch *arch;
			int impl;

			arch = _starpu_arch_comb_get(comb);
			for(impl = 0; impl < model->state->nimpls[comb]; impl++)
			{
				struct starpu_perfmodel_per_arch *arch_model = &model->state->per_arch[comb][impl];
				starpu_perfmodel_get_arch_name(arch, arch_name, 32, impl);

				if (arch_model->list)
				{
					print_comma(gnuplot_file, first);
					fprintf(gnuplot_file, "\"%s\" using 1:%d:%d with errorlines title \"Average %s\"", options->avg_file_name, col, col+1, replace_char(arch_name, '_', '-'));
					col += 2;
				}
			}
		}
	}

	/* Dump entries in size order */
	while (1)
	{
		last = minimum;

		minimum = ULONG_MAX;
		/* Get the next minimum */
		for(i = 0; i < model->state->ncombs; i++)
		{
			int comb = model->state->combs[i];
			if (options->comb_is_set == 0 || options->comb == comb)
			{
				int impl;
				for(impl = 0; impl < model->state->nimpls[comb]; impl++)
				{
					struct starpu_perfmodel_per_arch *arch_model = &model->state->per_arch[comb][impl];
					for (ptr = arch_model->list; ptr; ptr = ptr->next)
					{
						unsigned long size = ptr->entry->size;
						if (size > last && size < minimum)
							minimum = size;
					}
				}
			}
		}
		if (minimum == ULONG_MAX)
			break;

		fprintf(stderr, "%lu ", minimum);
		fprintf(datafile, "%-15lu ", minimum);
		for(i = 0; i < model->state->ncombs; i++)
		{
			int comb = model->state->combs[i];
			if (options->comb_is_set == 0 || options->comb == comb)
			{
				int impl;

				for(impl = 0; impl < model->state->nimpls[comb]; impl++)
				{
					struct starpu_perfmodel_per_arch *arch_model = &model->state->per_arch[comb][impl];
					for (ptr = arch_model->list; ptr; ptr = ptr->next)
					{
						struct starpu_perfmodel_history_entry *entry = ptr->entry;
						if (entry->size == minimum)
						{
							if (options->gflops)
								fprintf(datafile, "\t%-15le\t%-15le", entry->flops / (entry->mean * 1000),
									entry->flops / ((entry->mean + entry->deviation) * 1000) -
									entry->flops / (entry->mean * 1000)
									);
							else
								fprintf(datafile, "\t%-15le\t%-15le", 0.001*entry->mean, 0.001*entry->deviation);
							break;
						}
					}
					if (!ptr && arch_model->list)
						/* No value for this arch. */
						fprintf(datafile, "\t\"\"\t\"\"");
				}
			}
		}
		fprintf(datafile, "\n");
	}
	fprintf(stderr, "\n");

	fclose(datafile);
}

static void display_all_perf_models(FILE *gnuplot_file, struct starpu_perfmodel *model, int *first, struct _perfmodel_plot_options *options)
{
	int i;
	for(i = 0; i < model->state->ncombs; i++)
	{
		int comb = model->state->combs[i];
		if (options->comb_is_set == 0 || options->comb == comb)
		{
			struct starpu_perfmodel_arch *arch;
			int impl;

			arch = _starpu_arch_comb_get(comb);
			for(impl = 0; impl < model->state->nimpls[comb]; impl++)
			{
				struct starpu_perfmodel_per_arch *archmodel = &model->state->per_arch[comb][impl];
				display_perf_model(gnuplot_file, arch, archmodel, impl, first, options);
			}
		}
	}
}

#ifdef STARPU_USE_FXT
static void dump_data_file(FILE *data_file, struct _perfmodel_plot_options *options)
{
	int i;
	for (i = 0; i < options->fxt_options.dumped_codelets_count; i++)
	{
		/* Dump only if the codelet symbol matches user's request (with or without the machine name) */
		char *tmp = strdup(options->symbol);
		char *dot = strchr(tmp, '.');
		if (dot) tmp[strlen(tmp)-strlen(dot)] = '\0';
		if ((strncmp(options->dumped_codelets[i].symbol, options->symbol, (FXT_MAX_PARAMS - 4)*sizeof(unsigned long)-1) == 0)
		    || (strncmp(options->dumped_codelets[i].symbol, tmp, (FXT_MAX_PARAMS - 4)*sizeof(unsigned long)-1) == 0))
		{
			char *archname = options->dumped_codelets[i].perfmodel_archname;
			size_t size = options->dumped_codelets[i].size;
			float time = options->dumped_codelets[i].time;

			fprintf(data_file, "%s	%f	%f\n", archname, (float)size, time);
		}
		free(tmp);
	}
}
#endif

static void display_selected_models(FILE *gnuplot_file, struct starpu_perfmodel *model, struct _perfmodel_plot_options *options)
{
	fprintf(gnuplot_file, "#!/usr/bin/gnuplot -persist\n");
	fprintf(gnuplot_file, "\n");
	fprintf(gnuplot_file, "set term postscript eps enhanced color\n");
	fprintf(gnuplot_file, "set output \"starpu_%s.eps\"\n", options->symbol);
	fprintf(gnuplot_file, "set title \"Model for codelet %s\"\n", replace_char(options->symbol, '_', '-'));
	fprintf(gnuplot_file, "set xlabel \"Total data size\"\n");
	if (options->gflops)
		fprintf(gnuplot_file, "set ylabel \"GFlops\"\n");
	else
		fprintf(gnuplot_file, "set ylabel \"Time (ms)\"\n");
	fprintf(gnuplot_file, "\n");
	fprintf(gnuplot_file, "set key top left\n");
	fprintf(gnuplot_file, "set logscale x\n");
	fprintf(gnuplot_file, "set logscale y\n");
	fprintf(gnuplot_file, "\n");

	/* If no input data is given to gnuplot, we at least need to specify an
	 * arbitrary range. */
	if (options->with_fxt_file == 0)
		fprintf(gnuplot_file, "set xrange [1:10**9]\n\n");

	int first = 1;
	fprintf(gnuplot_file, "plot\t");

	/* display all or selected combinations */
	display_all_perf_models(gnuplot_file, model, &first, options);
	display_history_based_perf_models(gnuplot_file, model, &first, options);
}

int main(int argc, char **argv)
{
	int ret = 0;
	struct starpu_perfmodel model = {};
	char gnuplot_file_name[256];
	struct _perfmodel_plot_options options;

#if defined(_WIN32) && !defined(__CYGWIN__)
	WSADATA wsadata;
	WSAStartup(MAKEWORD(1,0), &wsadata);
#endif

	parse_args(argc, argv, &options);

        if (options.list)
	{
                ret = starpu_perfmodel_list(stdout);
                if (ret)
		{
                        fprintf(stderr, "The performance model directory is invalid\n");
                        return 1;
                }
		return 0;
        }

	/* Load the performance model associated to the symbol */
	ret = starpu_perfmodel_load_symbol(options.symbol, &model);
	if (ret == 1)
	{
		fprintf(stderr, "The performance model for the symbol <%s> could not be loaded\n", options.symbol);
		return 1;
	}

        if (options.list_combs)
	{
		ret = starpu_perfmodel_list_combs(stdout, &model);
                if (ret)
		{
                        fprintf(stderr, "Error when listing combinations for model <%s>\n", options.symbol);
                        return 1;
                }
		return 0;

	}

	/* If some FxT input was specified, we put the points on the graph */
#ifdef STARPU_USE_FXT
	if (options.with_fxt_file)
	{
		starpu_fxt_generate_trace(&options.fxt_options);

		snprintf(options.data_file_name, 256, "starpu_%s.data", options.symbol);

		FILE *data_file = fopen(options.data_file_name, "w+");
		STARPU_ASSERT(data_file);
		dump_data_file(data_file, &options);
		fclose(data_file);
	}
#endif

	snprintf(gnuplot_file_name, 256, "starpu_%s.gp", options.symbol);
	snprintf(options.avg_file_name, 256, "starpu_%s_avg.data", options.symbol);

	FILE *gnuplot_file = fopen(gnuplot_file_name, "w+");
	STARPU_ASSERT(gnuplot_file);
	display_selected_models(gnuplot_file, &model, &options);
	fprintf(gnuplot_file,"\n");
	fclose(gnuplot_file);

	/* Retrieve the current mode of the gnuplot executable */
	struct stat sb;
	ret = stat(gnuplot_file_name, &sb);
	if (ret)
	{
		perror("stat");
		STARPU_ABORT();
	}

	/* Make the gnuplot scrit executable for the owner */
	ret = chmod(gnuplot_file_name, sb.st_mode|S_IXUSR);
	if (ret)
	{
		perror("chmod");
		STARPU_ABORT();
	}

	_STARPU_DISP("Gnuplot file <%s> generated\n", gnuplot_file_name);

	return 0;
}
