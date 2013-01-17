/*
 * Copyright 2010,2011 Red Hat Inc., Durham, North Carolina.
 * All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Authors:
 *      Peter Vrabec   <pvrabec@redhat.com>
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <oval_probe.h>
#include <oval_agent_api.h>
#include <oval_agent_xccdf_api.h>
#include <oval_results.h>
#include <oval_variables.h>

#include <scap_ds.h>
#include <xccdf_benchmark.h>
#include <xccdf_policy.h>
#include <xccdf_session.h>
#include <oscap_acquire.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <assert.h>
#include <limits.h>
#include <unistd.h>
#include <syslog.h>

#include "oscap-tool.h"
#include "oscap.h"

static int app_evaluate_xccdf(const struct oscap_action *action);
static int app_xccdf_validate(const struct oscap_action *action);
static int app_xccdf_resolve(const struct oscap_action *action);
static int app_xccdf_export_oval_variables(const struct oscap_action *action);
static bool getopt_xccdf(int argc, char **argv, struct oscap_action *action);
static bool getopt_generate(int argc, char **argv, struct oscap_action *action);
static int xccdf_gen_report(const char *infile, const char *id, const char *outfile, const char *show, const char *oval_template, const char* sce_template, const char* profile);
static int app_xccdf_xslt(const struct oscap_action *action);

static struct oscap_module* XCCDF_SUBMODULES[];
static struct oscap_module* XCCDF_GEN_SUBMODULES[];

struct oscap_module OSCAP_XCCDF_MODULE = {
    .name = "xccdf",
    .parent = &OSCAP_ROOT_MODULE,
    .summary = "eXtensible Configuration Checklist Description Format",
    .usage_extra = "command [command-specific-options]",
    .submodules = XCCDF_SUBMODULES
};

static struct oscap_module XCCDF_RESOLVE = {
    .name = "resolve",
    .parent = &OSCAP_XCCDF_MODULE,
    .summary = "Resolve an XCCDF document",
    .usage = "[options] -o output-xccdf.xml input-xccdf.xml",
    .help =
        "Options:\n"
        "   --force or -f\r\t\t\t\t - Force resolving XCCDF document even if it is aleready marked as resolved.",
    .opt_parser = getopt_xccdf,
    .func = app_xccdf_resolve
};

static struct oscap_module XCCDF_VALIDATE_XML = {
    .name = "validate-xml",
    .parent = &OSCAP_XCCDF_MODULE,
    .summary = "Validate XCCDF XML content",
    .usage = "xccdf-file.xml",
    .opt_parser = getopt_xccdf,
    .func = app_xccdf_validate
};

static struct oscap_module XCCDF_VALIDATE = {
    .name = "validate",
    .parent = &OSCAP_XCCDF_MODULE,
    .summary = "Validate XCCDF XML content",
    .usage = "xccdf-file.xml",
    .opt_parser = getopt_xccdf,
    .func = app_xccdf_validate
};

static struct oscap_module XCCDF_EXPORT_OVAL_VARIABLES = {
    .name = "export-oval-variables",
    .parent = &OSCAP_XCCDF_MODULE,
    .summary = "Export XCCDF values as OVAL external-variables document(s)",
    .usage = "[options] <xccdf benchmark file> [oval definitions files]",
    .opt_parser = getopt_xccdf,
    .func = app_xccdf_export_oval_variables,
	.help =	"Options:\n"
		"   --profile <name>\r\t\t\t\t - The name of Profile to be evaluated.\n"
		"   --skip-valid \r\t\t\t\t - Skip validation.\n"
		"   --fetch-remote-resources \r\t\t\t\t - Download remote content referenced by XCCDF.\n"
		"   --datastream-id <id> \r\t\t\t\t - ID of the datastream in the collection to use.\n"
		"                        \r\t\t\t\t   (only applicable for source datastreams)\n"
		"   --xccdf-id <id> \r\t\t\t\t - ID of XCCDF in the datastream that should be evaluated.\n"
		"                   \r\t\t\t\t   (only applicable for source datastreams)",
};

static struct oscap_module XCCDF_EVAL = {
    .name = "eval",
    .parent = &OSCAP_XCCDF_MODULE,
    .summary = "Perform evaluation driven by XCCDF file and use OVAL as checking engine",
    .usage = "[options] INPUT_FILE [oval-definitions-files]",
    .help =
		"INPUT_FILE - XCCDF file or a source data stream file\n\n"
        "Options:\n"
        "   --profile <name>\r\t\t\t\t - The name of Profile to be evaluated.\n"
        "   --cpe <name>\r\t\t\t\t - Use given CPE dictionary or language (autodetected)\n"
        "               \r\t\t\t\t   for applicability checks.\n"
        "   --oval-results\r\t\t\t\t - Save OVAL results as well.\n"
#ifdef ENABLE_SCE
        "   --sce-results\r\t\t\t\t - Save SCE results as well.\n"
#endif
        "   --export-variables\r\t\t\t\t - Export OVAL external variables provided by XCCDF.\n"
        "   --results <file>\r\t\t\t\t - Write XCCDF Results into file.\n"
        "   --results-arf <file>\r\t\t\t\t - Write ARF (result data stream) into file.\n"
        "   --report <file>\r\t\t\t\t - Write HTML report into file.\n"
        "   --skip-valid \r\t\t\t\t - Skip validation.\n"
	"   --fetch-remote-resources \r\t\t\t\t - Download remote content referenced by XCCDF.\n"
	"   --progress \r\t\t\t\t - Switch to sparse output suitable for progress reporting.\n"
	"              \r\t\t\t\t   Format is \"$rule_id:$result\\n\".\n"
        "   --datastream-id <id> \r\t\t\t\t - ID of the datastream in the collection to use.\n"
        "                        \r\t\t\t\t   (only applicable for source datastreams)\n"
        "   --xccdf-id <id> \r\t\t\t\t - ID of XCCDF in the datastream that should be evaluated.\n"
        "                   \r\t\t\t\t   (only applicable for source datastreams)",
    .opt_parser = getopt_xccdf,
    .func = app_evaluate_xccdf
};

#define GEN_OPTS \
        "Generate options:\n" \
        "   --profile <profile-id>\r\t\t\t\t - Tailor XCCDF file with respect to a profile.\n" \
        "   --format <fmt>\r\t\t\t\t - Select output format. Can be html or docbook.\n"

static struct oscap_module XCCDF_GENERATE = {
    .name = "generate",
    .parent = &OSCAP_XCCDF_MODULE,
    .summary = "Convert XCCDF Benchmark to other formats",
    .usage = "[options]",
    .usage_extra = "<subcommand> [sub-options] benchmark-file.xml",
    .help = GEN_OPTS,
    .opt_parser = getopt_generate,
    .submodules = XCCDF_GEN_SUBMODULES
};

static struct oscap_module XCCDF_GEN_REPORT = {
    .name = "report",
    .parent = &XCCDF_GENERATE,
    .summary = "Generate results report",
    .usage = "[options] xccdf-file.xml",
    .help = GEN_OPTS
        "\nReport Options:\n"
        "   --result-id <id>\r\t\t\t\t - TestResult ID to be processed. Default is the most recent one.\n"
        "   --show <result-type*>\r\t\t\t\t - Rule results to show. Defaults to everything but notselected and notapplicable.\n"
        "   --output <file>\r\t\t\t\t - Write the document into file.\n"
        "   --oval-template <template-string> - Template which will be used to obtain OVAL result files.\n",
    .opt_parser = getopt_xccdf,
    .user = "xccdf-report.xsl",
    .func = app_xccdf_xslt
};

static struct oscap_module XCCDF_GEN_GUIDE = {
    .name = "guide",
    .parent = &XCCDF_GENERATE,
    .summary = "Generate security guide",
    .usage = "[options] xccdf-file.xml",
    .help = GEN_OPTS
        "\nGuide Options:\n"
        "   --output <file>\r\t\t\t\t - Write the document into file.\n"
        "   --hide-profile-info\r\t\t\t\t - Do not output additional information about selected profile.\n",
    .opt_parser = getopt_xccdf,
    .user = "security-guide.xsl",
    .func = app_xccdf_xslt
};

static struct oscap_module XCCDF_GEN_FIX = {
    .name = "fix",
    .parent = &XCCDF_GENERATE,
    .summary = "Generate a fix script from an XCCDF file",
    .usage = "[options] xccdf-file.xml",
    .help = GEN_OPTS
        "\nFix Options:\n"
        "   --output <file>\r\t\t\t\t - Write the script into file.\n"
        "   --result-id <id>\r\t\t\t\t - Fixes will be generated for failed rule-results of the specified TestResult.\n"
        "   --template <id|filename>\r\t\t\t\t - Fix template. (default: bash)\n",
    .opt_parser = getopt_xccdf,
    .user = "fix.xsl",
    .func = app_xccdf_xslt
};

static struct oscap_module XCCDF_GEN_CUSTOM = {
    .name = "custom",
    .parent = &XCCDF_GENERATE,
    .summary = "Generate a custom output (depending on given XSLT file) from an XCCDF file",
    .usage = "--stylesheet <file> [--output <file>] xccdf-file.xml",
    .help = GEN_OPTS
        "\nCustom Options:\n"
        "   --stylesheet <file>\r\t\t\t\t - Specify an absolute path to a custom stylesheet to format the output.\n"
        "   --output <file>\r\t\t\t\t - Write the document into file.\n",
    .opt_parser = getopt_xccdf,
    .user = NULL,
    .func = app_xccdf_xslt
};

static struct oscap_module* XCCDF_GEN_SUBMODULES[] = {
    &XCCDF_GEN_REPORT,
    &XCCDF_GEN_GUIDE,
    &XCCDF_GEN_FIX,
    &XCCDF_GEN_CUSTOM,
    NULL
};

static struct oscap_module* XCCDF_SUBMODULES[] = {
    &XCCDF_EVAL,
    &XCCDF_RESOLVE,
    &XCCDF_VALIDATE,
    &XCCDF_VALIDATE_XML,
    &XCCDF_EXPORT_OVAL_VARIABLES,
    &XCCDF_GENERATE,
    NULL
};

/**
 * XCCDF Result Colors:
 * PASS:green(32), FAIL:red(31), ERROR:lred(1;31), UNKNOWN:grey(1;30), NOT_APPLICABLE:white(1;37), NOT_CHECKED:white(1;37),
 * NOT_SELECTED:white(1;37), INFORMATIONAL:blue(34), FIXED:yellow(1;33)
 */
static const char * RESULT_COLORS[] = {"", "32", "31", "1;31", "1;30", "1;37", "1;37", "1;37", "34", "1;33" };

static char custom_stylesheet_path[PATH_MAX];

static int callback_scr_rule(struct xccdf_rule *rule, void *arg)
{
	const char * rule_id = xccdf_rule_get_id(rule);

	/* is rule selected? we print only selected rules */
	bool selected = xccdf_policy_is_item_selected((struct xccdf_policy *) arg, rule_id);
        if (!selected)
		return 0;

	/* get the first title */
	const char * title = NULL;
	struct oscap_text_iterator * title_it = xccdf_rule_get_title(rule);
	if (oscap_text_iterator_has_more(title_it))
		title = oscap_text_get_text(oscap_text_iterator_next(title_it));
	oscap_text_iterator_free(title_it);

	/* get the first ident */
	const char * ident_id = NULL;
	struct xccdf_ident_iterator *idents = xccdf_rule_get_idents(rule);
	if (xccdf_ident_iterator_has_more(idents)) {
		const struct xccdf_ident *ident = xccdf_ident_iterator_next(idents);
		ident_id = xccdf_ident_get_id(ident);
	}
	xccdf_ident_iterator_free(idents);

	/* print */
	if (isatty(1))
		printf("Title\r\t\033[1m%s\033[0;0m\n", title);
	else
		printf("Title\r\t%s\n", title);
	printf("Rule\r\t%s\n", rule_id);
	printf("Ident\r\t%s\n", ident_id);
	printf("Result\r\t");
	fflush(stdout);

	return 0;
}

static int callback_scr_result(struct xccdf_rule_result *rule_result, void *arg)
{
	xccdf_test_result_type_t result = xccdf_rule_result_get_result(rule_result);

	/* is result from selected rule? we print only selected rules */
	if (result == XCCDF_RESULT_NOT_SELECTED)
		return 0;

	/* print result */
	const char * result_str = xccdf_test_result_type_get_text(result);
	if (isatty(1))
		printf("\033[%sm%s\033[0m\n\n", RESULT_COLORS[result], result_str);
	else
		printf("%s\n\n", result_str);

	return 0;
}

static int callback_scr_rule_progress(struct xccdf_rule *rule, void *arg)
{
	const char * rule_id = xccdf_rule_get_id(rule);

	/* is rule selected? we print only selected rules */
	const bool selected = xccdf_policy_is_item_selected((struct xccdf_policy *) arg, rule_id);
	if (!selected)
		return 0;

	printf("%s:", rule_id);
	fflush(stdout);

	return 0;
}

static int callback_scr_result_progress(struct xccdf_rule_result *rule_result, void *arg)
{
	xccdf_test_result_type_t result = xccdf_rule_result_get_result(rule_result);

	/* is result from selected rule? we print only selected rules */
	if (result == XCCDF_RESULT_NOT_SELECTED)
		return 0;

	/* print result */
	const char * result_str = xccdf_test_result_type_get_text(result);

	printf("%s\n", result_str);
	fflush(stdout);

	return 0;
}

/*
 * Send XCCDF Rule Results info message to syslog
 *
static int callback_syslog_result(struct xccdf_rule_result *rule_result, void *arg)
{
	xccdf_test_result_type_t result = xccdf_rule_result_get_result(rule_result);

	// do we log it?
	if ((result != XCCDF_RESULT_FAIL) && (result != XCCDF_RESULT_UNKNOWN))
		return 0;

	// yes we do
	const char * result_str = xccdf_test_result_type_get_text(result);
	const char * ident_id = NULL;
	int priority = LOG_NOTICE;

	// get ident
	struct xccdf_ident_iterator *idents = xccdf_rule_result_get_idents(rule_result);
	if (xccdf_ident_iterator_has_more(idents)) {
		const struct xccdf_ident *ident = xccdf_ident_iterator_next(idents);
		ident_id = xccdf_ident_get_id(ident);
	}
	xccdf_ident_iterator_free(idents);

	// emit the message
	syslog(priority, "Rule: %s, Ident: %s, Result: %s.", xccdf_rule_result_get_idref(rule_result), ident_id, result_str);

	return 0;
}
*/


static void _download_reporting_callback(bool warning, const char *format, ...)
{
	FILE *dest = warning ? stderr : stdout;
	va_list argptr;
	va_start(argptr, format);
	vfprintf(dest, format, argptr);
	va_end(argptr);
	fflush(dest);
}

/**
 * XCCDF Processing fucntion
 * @param action OSCAP Action structure
 * @param sess OVAL Agent Session
 */
int app_evaluate_xccdf(const struct oscap_action *action)
{
	struct xccdf_session *session = NULL;

	struct xccdf_policy_model *policy_model = NULL;
	char* f_results = NULL;

	int result = OSCAP_ERROR;
	int priority = LOG_NOTICE;

	/* syslog message */
	syslog(priority, "Evaluation started. Content: %s, Profile: %s.", action->f_xccdf, action->profile);

	session = xccdf_session_new(action->f_xccdf);
	if (session == NULL)
		goto cleanup;
	xccdf_session_set_validation(session, action->validate, getenv("OSCAP_FULL_VALIDATION") != NULL);

	if (xccdf_session_is_sds(session)) {
		xccdf_session_set_datastream_id(session, action->f_datastream_id);
		xccdf_session_set_component_id(session, action->f_xccdf_id);
	}
	xccdf_session_set_user_cpe(session, action->cpe);
	xccdf_session_set_remote_resources(session, action->remote_resources, _download_reporting_callback);
	xccdf_session_set_custom_oval_files(session, action->f_ovals);
	xccdf_session_set_product_cpe(session, OSCAP_PRODUCTNAME);

	if (xccdf_session_load(session) != 0)
		goto cleanup;

	policy_model = xccdf_session_get_policy_model(session);

	/* Select profile */
	if (!xccdf_session_set_profile_id(session, action->profile)) {
		if (action->profile != NULL)
			fprintf(stderr, "Profile \"%s\" was not found.\n", action->profile);
		else
			fprintf(stderr, "No Policy was found for default profile.\n");
		goto cleanup;
	}

	/* Register callbacks */
	if (action->progress) {
		xccdf_policy_model_register_start_callback(policy_model, callback_scr_rule_progress,
				(void *) xccdf_session_get_xccdf_policy(session));
		xccdf_policy_model_register_output_callback(policy_model, callback_scr_result_progress, NULL);
	}
	else {
		xccdf_policy_model_register_start_callback(policy_model, callback_scr_rule,
				(void *) xccdf_session_get_xccdf_policy(session));
		xccdf_policy_model_register_output_callback(policy_model, callback_scr_result, NULL);
	}

	/* xccdf_policy_model_register_output_callback(policy_model, callback_syslog_result, NULL); */

	/* Perform evaluation */
	if (xccdf_session_evaluate(session) != 0)
		goto cleanup;

	xccdf_session_set_oval_results_export(session, action->oval_results);
	xccdf_session_set_oval_variables_export(session, action->export_variables);
	xccdf_session_set_arf_export(session, action->f_results_arf);

	if (xccdf_session_export_oval(session) != 0)
		goto cleanup;
	else if (action->validate && getenv("OSCAP_FULL_VALIDATION") != NULL &&
		(action->oval_results == true || action->f_results_arf))
		fprintf(stdout, "OVAL Results are exported correctly.\n");

#ifdef ENABLE_SCE
	xccdf_session_set_sce_results_export(session, action->sce_results);
	if (xccdf_session_export_sce(session) != 0)
		goto cleanup;
#endif

	f_results = action->f_results ? strdup(action->f_results) : NULL;
	if (!f_results && (action->f_report != NULL || action->f_results_arf != NULL))
	{
		if (!session->temp_dir)
			session->temp_dir = oscap_acquire_temp_dir();
		if (session->temp_dir == NULL)
			goto cleanup;

		f_results = malloc(PATH_MAX * sizeof(char));
		snprintf(f_results, PATH_MAX, "%s/xccdf-result.xml", session->temp_dir);
	}

	/* Export results */
	if (f_results != NULL) {
		xccdf_benchmark_add_result(xccdf_policy_model_get_benchmark(session->xccdf.policy_model),
				xccdf_result_clone(session->xccdf.result));
		xccdf_benchmark_export(xccdf_policy_model_get_benchmark(session->xccdf.policy_model), f_results);

		/* validate XCCDF Results */
		if (session->validate && session->full_validation) {
			/* we assume there is a same xccdf doc_version on input and output */
			if (oscap_validate_document(f_results, OSCAP_DOCUMENT_XCCDF, session->xccdf.doc_version, reporter, (void*) action)) {
				validation_failed(f_results, OSCAP_DOCUMENT_XCCDF, session->xccdf.doc_version);
				goto cleanup;
			}
			fprintf(stdout, "XCCDF Results are exported correctly.\n");
		}

		/* generate report */
		if (action->f_report != NULL)
			xccdf_gen_report(f_results,
			                 xccdf_result_get_id(session->xccdf.result),
			                 action->f_report,
			                 "",
			                 (action->oval_results ? "%.result.xml" : ""),
#ifdef ENABLE_SCE
			                 (action->sce_results  ? "%.result.xml" : ""),
#else
			                 "",
#endif
			                 action->profile == NULL ? "" : action->profile
			);
	}

	if (action->f_results_arf != NULL)
	{
		char* sds_path = 0;

		if (xccdf_session_is_sds(session))
		{
			sds_path = strdup(session->filename);
		}
		else
		{
			if (!session->temp_dir)
				session->temp_dir = oscap_acquire_temp_dir();
			if (session->temp_dir == NULL)
				goto cleanup;

			sds_path =  malloc(PATH_MAX * sizeof(char));
			snprintf(sds_path, PATH_MAX, "%s/sds.xml", session->temp_dir);
			ds_sds_compose_from_xccdf(session->filename, sds_path);
		}

		ds_rds_create(sds_path, f_results, (const char**)(session->oval.result_files), action->f_results_arf);
		free(sds_path);

		if (session->full_validation)
		{
			if (oscap_validate_document(action->f_results_arf, OSCAP_DOCUMENT_ARF, "1.1", reporter, (void*)action))
			{
				validation_failed(action->f_results_arf, OSCAP_DOCUMENT_ARF, "1.1");
				result = OSCAP_ERROR;
				goto cleanup;
			}
			fprintf(stdout, "Result DataStream exported correctly.\n");
		}
	}

	/* Get the result from TestResult model and decide if end with error or with correct return code */
	result = OSCAP_OK;
	struct xccdf_rule_result_iterator *res_it = xccdf_result_get_rule_results(session->xccdf.result);
	while (xccdf_rule_result_iterator_has_more(res_it)) {
		struct xccdf_rule_result *res = xccdf_rule_result_iterator_next(res_it);
		xccdf_test_result_type_t rule_result = xccdf_rule_result_get_result(res);
		if ((rule_result == XCCDF_RESULT_FAIL) || (rule_result == XCCDF_RESULT_UNKNOWN))
			result = OSCAP_FAIL;
	}
	xccdf_rule_result_iterator_free(res_it);


cleanup:
	if (oscap_err())
		fprintf(stderr, "%s %s\n", OSCAP_ERR_MSG, oscap_err_desc());

	/* syslog message */
	syslog(priority, "Evaluation finnished. Return code: %d, Base score %f.", result, xccdf_session_get_base_score(session));

	free(f_results);

	if (session != NULL)
		xccdf_session_free(session);

	return result;
}

static xccdf_test_result_type_t resolve_variables_wrapper(struct xccdf_policy *policy, const char *rule_id,
	const char *id, const char *href, struct xccdf_value_binding_iterator *bnd_itr,
	struct xccdf_check_import_iterator *check_import_it, void *usr)
{
	if (0 != oval_agent_resolve_variables((struct oval_agent_session *) usr, bnd_itr))
		return XCCDF_RESULT_UNKNOWN;

	return XCCDF_RESULT_PASS;
}

// todo: consolidate with app_evaluate_xccdf()
static int app_xccdf_export_oval_variables(const struct oscap_action *action)
{
	struct xccdf_policy *policy = NULL;
	struct xccdf_result *xres;
	int result = OSCAP_ERROR;
	struct xccdf_session *session = NULL;

	session = xccdf_session_new(action->f_xccdf);
	if (session == NULL)
		goto cleanup;

	xccdf_session_set_validation(session, action->validate, getenv("OSCAP_FULL_VALIDATION") != NULL);

	if (xccdf_session_is_sds(session)) {
		xccdf_session_set_datastream_id(session, action->f_datastream_id);
		xccdf_session_set_component_id(session, action->f_xccdf_id);
	}
	xccdf_session_set_remote_resources(session, action->remote_resources, _download_reporting_callback);
	xccdf_session_set_custom_oval_files(session, action->f_ovals);
	xccdf_session_set_custom_oval_eval_fn(session, resolve_variables_wrapper);

	if (xccdf_session_load_xccdf(session) != 0)
		goto cleanup;

	if (xccdf_session_load_oval(session) != 0)
		goto cleanup;

	/* select a profile */
	policy = xccdf_policy_model_get_policy_by_id(xccdf_session_get_policy_model(session), action->profile);
	if (policy == NULL) {
		if (action->profile != NULL)
			fprintf(stderr, "Profile \"%s\" was not found.\n", action->profile);
		else
			fprintf(stderr, "No Policy was found for default profile.\n");
		goto cleanup;
	}

	/* perform evaluation */
	xres = xccdf_policy_evaluate(policy);
	if (xres == NULL)
		goto cleanup;

	xccdf_session_set_oval_variables_export(session, true);
	xccdf_session_export_oval(session);

	result = OSCAP_OK;

 cleanup:
	if (oscap_err())
		fprintf(stderr, "%s %s\n", OSCAP_ERR_MSG, oscap_err_desc());

	if (session != NULL)
		xccdf_session_free(session);

	return result;
}

int app_xccdf_resolve(const struct oscap_action *action)
{
	char *doc_version = NULL;
	int ret = OSCAP_ERROR;
	struct xccdf_benchmark *bench = NULL;

	if (!action->f_xccdf) {
		fprintf(stderr, "No input document specified!\n");
		return OSCAP_ERROR;
	}
	if (!action->f_results) {
		fprintf(stderr, "No output document filename specified!\n");
		return OSCAP_ERROR;
	}

	/* validate input */
	if (action->validate) {
		doc_version = xccdf_detect_version(action->f_xccdf);
		if (!doc_version) {
			return OSCAP_ERROR;
		}

		if (oscap_validate_document(action->f_xccdf, OSCAP_DOCUMENT_XCCDF, doc_version, reporter, (void*) action) != 0) {
			validation_failed(action->f_xccdf, OSCAP_DOCUMENT_XCCDF, doc_version);
			goto cleanup;
		}
	}

	bench = xccdf_benchmark_import(action->f_xccdf);
	if (!bench)
		goto cleanup;

	if (action->force)
		xccdf_benchmark_set_resolved(bench, false);

	if (xccdf_benchmark_get_resolved(bench))
		fprintf(stderr, "Benchmark is already resolved!\n");
	else {
		if (!xccdf_benchmark_resolve(bench))
			fprintf(stderr, "Benchmark resolving failure (probably a dependency loop)!\n");

		{
			if (xccdf_benchmark_export(bench, action->f_results)) {
				ret = OSCAP_OK;

				/* validate exported results */
				const char* full_validation = getenv("OSCAP_FULL_VALIDATION");
				if (action->validate && full_validation) {
					/* reuse doc_version from unresolved document
					   it should be same in resolved one */
					if (oscap_validate_document(action->f_results, OSCAP_DOCUMENT_XCCDF, doc_version, reporter, (void*)action)) {
						validation_failed(action->f_results, OSCAP_DOCUMENT_XCCDF, doc_version);
						ret = OSCAP_ERROR;
					}
					else
						fprintf(stdout, "Resolved XCCDF has been exported correctly.\n");
				}
			}
		}
	}

cleanup:
	if (oscap_err())
		fprintf(stderr, "Error: %s\n", oscap_err_desc());
	if (bench)
		xccdf_benchmark_free(bench);
	if (doc_version)
		free(doc_version);

	return ret;
}

static int xccdf_gen_report(const char *infile, const char *id, const char *outfile, const char *show, const char *oval_template, const char *sce_template, const char* profile)
{
	const char *params[] = {
		"result-id",         id,
		"show",              show,
		"profile",           profile,
		"oval-template",     oval_template,
		"sce-template",      sce_template,
		"verbosity",         "",
		"hide-profile-info", NULL,
		NULL };

	return app_xslt(infile, "xccdf-report.xsl", outfile, params);
}

static bool _some_oval_result_exists(const char *filename)
{
	struct xccdf_benchmark *benchmark = NULL;
	struct xccdf_policy_model *policy_model = NULL;
	struct oscap_file_entry_list *files = NULL;
	struct oscap_file_entry_iterator *files_it = NULL;
	char *oval_result = NULL;
	bool result = false;

	benchmark = xccdf_benchmark_import(filename);
	if (benchmark == NULL)
		return false;

	policy_model = xccdf_policy_model_new(benchmark);
	files = xccdf_policy_model_get_systems_and_files(policy_model);
	files_it = oscap_file_entry_list_get_files(files);
	oval_result = malloc(PATH_MAX * sizeof(char));
	while (oscap_file_entry_iterator_has_more(files_it)) {
		struct oscap_file_entry *file_entry = (struct oscap_file_entry *) oscap_file_entry_iterator_next(files_it);;
		struct stat sb;
		if (strcmp(oscap_file_entry_get_system(file_entry), "http://oval.mitre.org/XMLSchema/oval-definitions-5"))
			continue;
		snprintf(oval_result, PATH_MAX, "./%s.result.xml", oscap_file_entry_get_file(file_entry));
		if (stat(oval_result, &sb) == 0) {
			result = true;
			break;
		}
	}
	free(oval_result);
	oscap_file_entry_iterator_free(files_it);
	oscap_file_entry_list_free(files);
	xccdf_policy_model_free(policy_model);
	return result;
}

int app_xccdf_xslt(const struct oscap_action *action)
{
	const char *oval_template = action->oval_template;

	if (action->module == &XCCDF_GEN_REPORT && oval_template == NULL) {
		/* If generating the report and the option is missing -> use defaults */
		if (_some_oval_result_exists(action->f_xccdf))
			/* We want to define default template because we strive to serve user the
			 * best. However, we must not offer a template, if there is a risk it might
			 * be incorrect. Otherwise, libxml2 will throw a lot of misleading messages
			 * to stderr. */
			oval_template = "%.result.xml";
	}

	if (action->module == &XCCDF_GEN_CUSTOM) {
	        action->module->user = (void*)action->stylesheet;
	}

	const char *params[] = {
		"result-id",         action->id,
		"show",              action->show,
		"profile",           action->profile,
		"template",          action->tmpl,
		"format",            action->format,
		"oval-template",     oval_template,
#ifdef ENABLE_SCE
		"sce-template",      action->sce_template,
#endif
		"verbosity",         "",
		"hide-profile-info", action->hide_profile_info ? "yes" : NULL,
		NULL
	};

	int ret = app_xslt(action->f_xccdf, action->module->user, action->f_results, params);
	return ret;
}

bool getopt_generate(int argc, char **argv, struct oscap_action *action)
{
	static const struct option long_options[] = {
		{"profile", 1, 0, 3},
		{"format", 1, 0, 4},
		{0, 0, 0, 0}
	};

	int c;
	while ((c = getopt_long(argc, argv, "+", long_options, NULL)) != -1) {
		switch (c) {
		case 3: action->profile = optarg; break;
		case 4: action->format = optarg; break;
		default: return oscap_module_usage(action->module, stderr, NULL);
		}
	}
    return true;
}

enum oval_opt {
    XCCDF_OPT_RESULT_FILE = 1,
    XCCDF_OPT_RESULT_FILE_ARF,
    XCCDF_OPT_DATASTREAM_ID,
    XCCDF_OPT_XCCDF_ID,
    XCCDF_OPT_PROFILE,
    XCCDF_OPT_REPORT_FILE,
    XCCDF_OPT_SHOW,
    XCCDF_OPT_TEMPLATE,
    XCCDF_OPT_FORMAT,
    XCCDF_OPT_OVAL_TEMPLATE,
    XCCDF_OPT_STYLESHEET_FILE,
#ifdef ENABLE_SCE
    XCCDF_OPT_SCE_TEMPLATE,
#endif
    XCCDF_OPT_FILE_VERSION,
    XCCDF_OPT_CPE,
    XCCDF_OPT_CPE_DICT,
    XCCDF_OPT_OUTPUT = 'o',
    XCCDF_OPT_RESULT_ID = 'i'
};

bool getopt_xccdf(int argc, char **argv, struct oscap_action *action)
{
	assert(action != NULL);

	action->doctype = OSCAP_DOCUMENT_XCCDF;

	/* Command-options */
	const struct option long_options[] = {
	// options
		{"output",		required_argument, NULL, XCCDF_OPT_OUTPUT},
		{"results", 		required_argument, NULL, XCCDF_OPT_RESULT_FILE},
		{"results-arf",		required_argument, NULL, XCCDF_OPT_RESULT_FILE_ARF},
		{"datastream-id",		required_argument, NULL, XCCDF_OPT_DATASTREAM_ID},
		{"xccdf-id",		required_argument, NULL, XCCDF_OPT_XCCDF_ID},
		{"profile", 		required_argument, NULL, XCCDF_OPT_PROFILE},
		{"result-id",		required_argument, NULL, XCCDF_OPT_RESULT_ID},
		{"report", 		required_argument, NULL, XCCDF_OPT_REPORT_FILE},
		{"show", 		required_argument, NULL, XCCDF_OPT_SHOW},
		{"template", 		required_argument, NULL, XCCDF_OPT_TEMPLATE},
		{"format", 		required_argument, NULL, XCCDF_OPT_FORMAT},
		{"oval-template", 	required_argument, NULL, XCCDF_OPT_OVAL_TEMPLATE},
		{"stylesheet",	required_argument, NULL, XCCDF_OPT_STYLESHEET_FILE},
		{"cpe",	required_argument, NULL, XCCDF_OPT_CPE},
		{"cpe-dict",	required_argument, NULL, XCCDF_OPT_CPE_DICT}, // DEPRECATED!
#ifdef ENABLE_SCE
		{"sce-template", 	required_argument, NULL, XCCDF_OPT_SCE_TEMPLATE},
#endif
	// flags
		{"force",		no_argument, &action->force, 1},
		{"oval-results",	no_argument, &action->oval_results, 1},
#ifdef ENABLE_SCE
		{"sce-results",	no_argument, &action->sce_results, 1},
#endif
		{"skip-valid",		no_argument, &action->validate, 0},
		{"fetch-remote-resources", no_argument, &action->remote_resources, 1},
		{"progress", no_argument, &action->progress, 1},
		{"hide-profile-info",	no_argument, &action->hide_profile_info, 1},
		{"export-variables",	no_argument, &action->export_variables, 1},
	// end
		{0, 0, 0, 0}
	};

	int c;
	while ((c = getopt_long(argc, argv, "o:i:", long_options, NULL)) != -1) {

		switch (c) {
		case XCCDF_OPT_OUTPUT:
		case XCCDF_OPT_RESULT_FILE:	action->f_results = optarg;	break;
		case XCCDF_OPT_RESULT_FILE_ARF:	action->f_results_arf = optarg;	break;
		case XCCDF_OPT_DATASTREAM_ID:	action->f_datastream_id = optarg;	break;
		case XCCDF_OPT_XCCDF_ID:	action->f_xccdf_id = optarg; break;
		case XCCDF_OPT_PROFILE:		action->profile = optarg;	break;
		case XCCDF_OPT_RESULT_ID:	action->id = optarg;		break;
		case XCCDF_OPT_REPORT_FILE:	action->f_report = optarg; 	break;
		case XCCDF_OPT_SHOW:		action->show = optarg;		break;
		case XCCDF_OPT_TEMPLATE:	action->tmpl = optarg;		break;
		case XCCDF_OPT_FORMAT:		action->format = optarg;	break;
		case XCCDF_OPT_OVAL_TEMPLATE:	action->oval_template = optarg; break;
		/* we use realpath to get an absolute path to given XSLT to prevent openscap from looking
		   into /usr/share/openscap/xsl instead of CWD */
		case XCCDF_OPT_STYLESHEET_FILE: realpath(optarg, custom_stylesheet_path); action->stylesheet = custom_stylesheet_path; break;
		case XCCDF_OPT_CPE:			action->cpe = optarg; break;
		case XCCDF_OPT_CPE_DICT:
			{
				fprintf(stdout, "Warning: --cpe-dict is a deprecated option. Please use --cpe instead!\n\n");
				action->cpe = optarg; break;
			}
#ifdef ENABLE_SCE
		case XCCDF_OPT_SCE_TEMPLATE:	action->sce_template = optarg; break;
#endif
		case 0: break;
		default: return oscap_module_usage(action->module, stderr, NULL);
		}
	}

	if (action->module == &XCCDF_EVAL) {
		/* We should have XCCDF file here */
		if (optind >= argc) {
			/* TODO */
			return oscap_module_usage(action->module, stderr, "XCCDF file need to be specified!");
		}

                action->f_xccdf = argv[optind];
                if (argc > (optind+1)) {
                    action->f_ovals = malloc((argc-(optind+1)+1) * sizeof(char *));
                    int i = 1;
                    while (argc > (optind+i)) {
                        action->f_ovals[i-1] = argv[optind + i];
                        i++;
                    }
                    action->f_ovals[i-1] = NULL;
                } else {
                    action->f_ovals = NULL;
                }
	} else if (action->module == &XCCDF_GEN_CUSTOM) {
		if (!action->stylesheet) {
			return oscap_module_usage(action->module, stderr, "XSLT Stylesheet needs to be specified!");
		}

		if (optind >= argc)
			return oscap_module_usage(action->module, stderr, "XCCDF file needs to be specified!");
		action->f_xccdf = argv[optind];
	} else {
		if (optind >= argc)
			return oscap_module_usage(action->module, stderr, "XCCDF file needs to be specified!");
		action->f_xccdf = argv[optind];
	}

	return true;
}

int app_xccdf_validate(const struct oscap_action *action) {
	int ret;
	char *doc_version;
	int result;


	doc_version = xccdf_detect_version(action->f_xccdf);
        if (!doc_version) {
                result = OSCAP_ERROR;
                goto cleanup;
        }

        ret=oscap_validate_document(action->f_xccdf, action->doctype, doc_version, reporter, (void*)action);
        if (ret==-1) {
                result=OSCAP_ERROR;
                goto cleanup;
        }
        else if (ret==1) {
                result=OSCAP_FAIL;
        }
        else
                result=OSCAP_OK;

        if (result==OSCAP_FAIL)
		validation_failed(action->f_xccdf, OSCAP_DOCUMENT_XCCDF, doc_version);

cleanup:
        if (oscap_err())
                fprintf(stderr, "%s %s\n", OSCAP_ERR_MSG, oscap_err_desc());

        if (doc_version)
		free(doc_version);

        return result;

}
