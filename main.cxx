/*
 * Copyright (c) 2019 Anatol Belski
 * All rights reserved.
 *
 * Author: Anatol Belski <ab@php.net>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *	notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *	notice, this list of conditions and the following disclaimer in the
 *	documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

/* {{{ Includes */
#include <windows.h>
#include <conio.h>

#include <iostream>
#include <thread>
#include <atomic>

#include "popl.hpp"
/* }}} */

/* {{{ Global decls */
static HANDLE pipe = INVALID_HANDLE_VALUE,
		in  = INVALID_HANDLE_VALUE,
		out = INVALID_HANDLE_VALUE,
		e0  = INVALID_HANDLE_VALUE,
		e1  = INVALID_HANDLE_VALUE;
static DWORD orig_ccp = 0, orig_cocp = 0;
static bool is_console = false;
static std::atomic<bool> is_error{false};

#define BUF_SIZE 4096
#define VERSION "0.0.1"

struct convey_conf {
	bool verbose;
	std::string pipe_path;
	double pipe_poll;
};

enum convey_setup_status {
	convey_setup_ok,
	convey_setup_exit_ok,
	convey_setup_exit_err
};
/*static convey_conf conf = {
	.verbose = false,
	.pipe_path = std::string(""),
	.pall = 0
};*/
static convey_conf conf = {0};

/* }}} */

/* {{{ Helper routines */
static void convey_error(DWORD c = -1)
{/*{{{*/
	char *buf = NULL;
	DWORD err_code = (-1 == c) ? GetLastError() : c;

	DWORD ret = FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL, err_code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&buf, 0, nullptr
	);

	if (ret) {
		std::cout << "convey: " << buf << std::endl;

		LocalFree(buf);
	}
}/*}}}*/



static DWORD convey_get_ov_result(HANDLE h, OVERLAPPED& ov, DWORD& bytes, bool rc, DWORD er)
{/*{{{*/
	if (!rc) {
		switch (er) {
		case ERROR_HANDLE_EOF:
			convey_error(er);
			return false;
			break;
		case ERROR_IO_PENDING:
			rc = GetOverlappedResult(h, &ov, &bytes, true);
			if (!rc) {
				er = GetLastError();
				switch (er) {
				case ERROR_HANDLE_EOF:
					convey_error(er);
					return false;
					break;
				case ERROR_IO_INCOMPLETE:
					// should not happen as we're waiting for the op to complete
					convey_error(er);
					return false;
					break;
				default:
					convey_error(er);
					return false;
				}
			}
			//ResetEvent(ov.hEvent);
			break;
		default:
			convey_error(er);
			return false;
		}
	} else {
		//ResetEvent(ov.hEvent);
	}

	return true;
}/*}}}*/

static void convey_usage_print(popl::OptionParser& op)
{
	std::cerr << "Usage: convey [options] \\\\.\\pipe\\<pipe name>" << std::endl;
	std::cout << op << std::endl;
}

static convey_setup_status convey_conf_setup(int argc, char **argv)
{/*{{{*/
	popl::OptionParser op{};
	auto help_opt = op.add<popl::Switch>("h", "help", "This help message.");
	auto pipe_path_opt = op.add<popl::Value<std::string>>("n", "pipe", "Named pipe path.");
	auto pipe_poll_unavail_opt = op.add<popl::Value<double>>("p", "poll", "Poll pipe for N seconds on startup.");
	auto verbose_opt = op.add<popl::Switch>("v", "verbose", "Print some additional messages.");

	try {
		op.parse(argc, argv);

		if (op.unknown_options().size() > 0) {
			for (const auto& unknown_option: op.unknown_options()) {
				std::cerr << argv[0] << ": unknown option '" << unknown_option << "'" << std::endl;
				std::cerr << "Try 'convey --help' for more information." << std::endl;
				return convey_setup_exit_err;
			}
		}

		if (help_opt->count() >= 1) {
			convey_usage_print(op);
			return convey_setup_exit_ok;
		}

		if (verbose_opt->count() >= 1) {
			conf.verbose = true;
		}

		if (pipe_path_opt->is_set()) {
			conf.pipe_path = pipe_path_opt->value();
		} else if (op.non_option_args().size() >= 1) {
			// If not passed explicitly by opt, pipe is passed just as a first arg.
			conf.pipe_path = op.non_option_args()[0];
		} else {
			std::cerr << argv[0] << ": empty pipe path" << std::endl;
			std::cerr << "Try 'convey --help' for more information." << std::endl;
			return convey_setup_exit_err;
		}

		if (pipe_poll_unavail_opt->is_set()) {
			conf.pipe_poll = pipe_poll_unavail_opt->value();
		}

	} catch (const popl::invalid_option& e) {
		std::cerr << "Invalid option " << e.what() << std::endl;
		return convey_setup_exit_err;
	}

	return convey_setup_ok;
}/*}}}*/

static bool convey_conf_shutdown()
{/*{{{*/
	return true;
}/*}}}*/

static void convey_shutdown(void);

static BOOL WINAPI ctrl_handler(DWORD sig)
{/*{{{*/
	convey_shutdown();
	return FALSE;
}/*}}}*/

static bool is_console_handle(HANDLE h)
{/*{{{*/
	DWORD mode;
	return GetConsoleMode(h, &mode);
}/*}}}*/

static void setup_console(void)
{/*{{{*/
	orig_ccp = GetConsoleCP();
	orig_cocp = GetConsoleOutputCP();
	SetConsoleOutputCP(65001U);
	SetConsoleCP(65001U);
	SetConsoleCtrlHandler(ctrl_handler, TRUE);
}/*}}}*/

static void restore_console(void)
{/*{{{*/
	SetConsoleOutputCP(orig_cocp);
	SetConsoleCP(orig_ccp);
}/*}}}*/

static convey_setup_status convey_startup(int argc, char **argv)
{/*{{{*/
	DWORD rc = -1;

	convey_setup_status _rc = convey_conf_setup(argc, argv);
	if(convey_setup_ok != _rc) {
		return _rc;
	}

	if (conf.verbose) {
		std::cout << "Polling the pipe '" << conf.pipe_path << "' for " << conf.pipe_poll << " seconds" << std::endl;
	}
	size_t elapsed = 0,
		   step = 300 /* milliseconds*/;
	do {
		pipe = CreateFile(conf.pipe_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
		rc = GetLastError();
		if (INVALID_HANDLE_VALUE == pipe || ERROR_PIPE_BUSY == rc) {
			if (elapsed/1000 < conf.pipe_poll) {
				std::this_thread::sleep_for(std::chrono::milliseconds(step));
				elapsed += step;
				continue;
			}
		}
		break;
	} while (true);
	if (INVALID_HANDLE_VALUE == pipe || ERROR_PIPE_BUSY == rc) {
		convey_error(rc);
		return convey_setup_exit_err;
	}
	if (conf.verbose) {
		std::cout << "Connection established in " << (elapsed/1000) << " seconds" << std::endl;
	}

	/* This could be something else, too. */
	in = GetStdHandle(STD_INPUT_HANDLE);
	if (INVALID_HANDLE_VALUE == in) {
		convey_error();
		convey_shutdown();
		return convey_setup_exit_err;
	}
	/* This could be something else, too. */
	out = GetStdHandle(STD_OUTPUT_HANDLE);
	if (INVALID_HANDLE_VALUE == out) {
		convey_error();
		convey_shutdown();
		return convey_setup_exit_err;
	}

	e0 = CreateEvent(nullptr, false, false, nullptr);
	e1 = CreateEvent(nullptr, false, false, nullptr);

	is_console = is_console_handle(in) && is_console_handle(out);

	if (is_console) {
		setup_console();
	}

	return convey_setup_ok;
}/*}}}*/

static void convey_shutdown(void)
{/*{{{*/
	if (INVALID_HANDLE_VALUE != pipe) {
		CloseHandle(pipe);
	}
	if (INVALID_HANDLE_VALUE != in) {
		CloseHandle(in);
	}
	if (INVALID_HANDLE_VALUE != out) {
		CloseHandle(out);
	}
	if (INVALID_HANDLE_VALUE != e0) {
		CloseHandle(e0);
	}
	if (INVALID_HANDLE_VALUE != e1) {
		CloseHandle(e1);
	}

	if (is_console) {
		restore_console();
	}

	convey_conf_shutdown();
}/*}}}*/
/* }}} */

int main(int argc, char** argv)
{/*{{{*/

	switch (convey_startup(argc, argv)) {
		case convey_setup_ok:
			// pass
			break;
		case convey_setup_exit_err:
			return 1;
		case convey_setup_exit_ok:
			return 0;
	}

	std::thread t0([]() {
		while (true) {
			if (is_error) {
				return;
			}

			char buf[BUF_SIZE];
			DWORD bytes = 0;

			if (!ReadFile(in, buf, sizeof buf, &bytes, nullptr)) {
				convey_error();
				is_error = true;
				return;
			}

			if (bytes) {
				OVERLAPPED ov = {0, 0, 0, 0, e0};

				// Cut out CRLF.
				// TODO parametrize this, if needed
				if (bytes >= 2 && '\n' == buf[bytes - 1] && '\r' == buf[bytes - 2]) {
					bytes -= 1;
				}

				bool rc = WriteFile(pipe, buf, bytes, &bytes, &ov);
				DWORD er = GetLastError();
				rc = convey_get_ov_result(pipe, ov, bytes, rc, er);
				if (!rc) {
					is_error = true;
					return;
				}
			}
		}
		return;
	});

	std::thread t1([]() {
		while (true) {
			if (is_error) {
				return;
			}

			char buf[BUF_SIZE];
			DWORD bytes = 0;
			OVERLAPPED ov = {0, 0, 0, 0, e1};
			
			bool rc = ReadFile(pipe, buf, sizeof buf, &bytes, &ov);
			DWORD er = GetLastError();
			rc = convey_get_ov_result(pipe, ov, bytes, rc, er);
			if (!rc) {
				is_error = true;
				return;
			}

			if (bytes && !WriteFile(out, buf, bytes, &bytes, nullptr)) {
				convey_error();
				is_error = true;
				return;
			}
		}
	});

	t0.join();
	t1.join();

	convey_shutdown();

	return 0;
}/*}}}*/

