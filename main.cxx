/*
 * Copyright (c) 2019-2026 Anatol Belski
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
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <conio.h>
#include <fcntl.h>
#include <io.h>

#include <cstring>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <atomic>

#include "popl.hpp"

#include "config.h"
/* }}} */

/* {{{ Global decls */
static HANDLE pipe{INVALID_HANDLE_VALUE},
			bpipe{INVALID_HANDLE_VALUE},
			in{INVALID_HANDLE_VALUE},
			out{INVALID_HANDLE_VALUE},
			e_pipe_w{INVALID_HANDLE_VALUE},
			e_pipe_r{INVALID_HANDLE_VALUE},
			e_in{INVALID_HANDLE_VALUE},
			e_out{INVALID_HANDLE_VALUE};
static DWORD orig_ccp{0},
			 orig_cocp{0},
			 orig_in_cmode{0},
			 orig_out_cmode{0};
static bool is_console{false},
			in_is_pipe{false},
			out_is_pipe{false};
static std::atomic<bool> is_error{false};
static std::atomic<bool> shutting_down{false};
static std::atomic<bool> ctrl_mode{false};
static bool restart_on_exit = false;
static SOCKET listen_sock{INVALID_SOCKET};
static HANDLE stdin_thread{INVALID_HANDLE_VALUE};
static HANDLE log_handle{INVALID_HANDLE_VALUE};
static HANDLE log_recv_handle{INVALID_HANDLE_VALUE};
static HANDLE log_send_handle{INVALID_HANDLE_VALUE};

#define BUF_SIZE 4096

enum convey_flow_control {
	convey_flow_control_none,
	convey_flow_control_xonxoff,
	convey_flow_control_rtscts,
	convey_flow_control_dsrdtr
};

enum convey_transport {
	convey_tp_pipe,
	convey_tp_serial,
	convey_tp_tcp_client,
	convey_tp_tcp_server
};

struct convey_transport_spec {
	convey_transport kind;
	std::string host;
	std::string port;
	bool ok;
};

static convey_transport_spec convey_parse_transport(const std::string& spec)
{/*{{{*/
	convey_transport_spec r{convey_tp_pipe, "", "", true};

	const std::string tcp_pfx = "tcp:";
	const std::string tcpl_pfx = "tcp-listen:";

	if (0 == spec.compare(0, tcpl_pfx.size(), tcpl_pfx)) {
		r.kind = convey_tp_tcp_server;
		r.port = spec.substr(tcpl_pfx.size());
		r.ok = !r.port.empty();
	} else if (0 == spec.compare(0, tcp_pfx.size(), tcp_pfx)) {
		r.kind = convey_tp_tcp_client;
		std::string hp = spec.substr(tcp_pfx.size());
		auto pos = hp.rfind(':');
		if (std::string::npos == pos || 0 == pos || pos + 1 == hp.size()) {
			r.ok = false;
		} else {
			r.host = hp.substr(0, pos);
			r.port = hp.substr(pos + 1);
		}
	}

	return r;
}/*}}}*/

static DWORD convey_trim_crlf(const char* buf, DWORD bytes)
{/*{{{*/
	if (bytes >= 2 && '\n' == buf[bytes - 1] && '\r' == buf[bytes - 2]) {
		return bytes - 1;
	}
	return bytes;
}/*}}}*/

struct convey_conf {
	bool verbose;
	bool no_xterm;
	std::string pipe_path;
	double pipe_poll;
	uint32_t baud;
	uint8_t parity;
	uint8_t stop_bits;
	uint8_t byte_size;
	convey_flow_control flow_control;
	convey_transport transport;
	std::string tcp_host;
	std::string tcp_port;
	bool bridge;
	std::string bridge_pipe_name;
	std::string log_path;
	std::string log_recv_path;
	std::string log_send_path;
	bool log_append;
};

static convey_conf conf{0};

enum convey_setup_status {
	convey_setup_ok,
	convey_setup_exit_ok,
	convey_setup_exit_err
};

/* }}} */

/* {{{ Helper routines */
static void convey_error(DWORD c = -1)
{/*{{{*/
	char *buf{nullptr};
	DWORD err_code = (-1 == c) ? GetLastError() : c;

	if (!conf.verbose && (ERROR_BROKEN_PIPE == err_code
			|| ERROR_NETNAME_DELETED == err_code
			|| ERROR_CONNECTION_ABORTED == err_code
			|| ERROR_OPERATION_ABORTED == err_code)) {
		return;
	}

	DWORD ret = FormatMessage(
		FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL, err_code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&buf, 0, nullptr
	);

	if (ret) {
		std::cerr << "convey: 0x" << std::hex << err_code << ": " << buf << std::endl;

		LocalFree(buf);
	}
}/*}}}*/

static DWORD convey_get_ov_result(HANDLE h, OVERLAPPED* ov, DWORD* bytes, bool& rc, DWORD& er)
{/*{{{*/
	if (!rc) {
		switch (er) {
		case ERROR_HANDLE_EOF:
			return false;
			break;
		case ERROR_IO_PENDING:
			rc = GetOverlappedResult(h, ov, bytes, true);
			if (!rc) {
				er = GetLastError();
				switch (er) {
				case ERROR_HANDLE_EOF:
					return false;
					break;
				case ERROR_IO_INCOMPLETE:
					// should not happen as we're waiting for the op to complete
					return false;
					break;
				default:
					return false;
				}
			}
			//ResetEvent(ov.hEvent);
			break;
		default:
			return false;
		}
	} else {
		//ResetEvent(ov.hEvent);
	}

	return true;
}/*}}}*/

static bool convey_baud_is_valid(uint32_t b)
{
	switch (b) {
		case CBR_110:
		case CBR_300:
		case CBR_600:
		case CBR_1200:
		case CBR_2400:
		case CBR_4800:
		case CBR_9600:
		case CBR_14400:
		case CBR_19200:
		case CBR_38400:
		case CBR_57600:
		case CBR_115200:
		case CBR_128000:
		case CBR_256000:
			return true;
		default:
			return false;
	}
}

static decltype(auto) convey_get_baud(std::shared_ptr<popl::Value<uint32_t>>& opt)
{
	uint32_t b;

	if (opt->is_set()) {
		b = opt->value();
		if (!convey_baud_is_valid(b)) {
			std::cerr << "convey: unsupported baud rate '" << b << "'" << std::endl;
			return ((decltype(b))-1);
		}
	} else {
		b = opt->get_default();
	}

	return b;
}

static uint8_t convey_parity_from_string(std::string p)
{
	for (size_t i = 0; i < p.size(); i++) {
		p[i] = std::tolower(p[i]);
	}
	if (!p.compare("even")) {
		return EVENPARITY;
	} else if (!p.compare("mark")) {
		return MARKPARITY;
	} else if (!p.compare("no")) {
		return NOPARITY;
	} else if (!p.compare("odd")) {
		return ODDPARITY;
	} else if (!p.compare("space")) {
		return SPACEPARITY;
	}
	return ((uint8_t)-1);
}

static decltype(auto) convey_get_parity(std::shared_ptr<popl::Value<std::string>>& opt)
{
	uint8_t ret = NOPARITY;

	if (opt->is_set()) {
		ret = convey_parity_from_string(opt->value());
		if (((decltype(ret))-1) == ret) {
			std::cerr << "convey: unsupported parity '" << opt->value() << "'" << std::endl;
		}
	}

	return ret;
}

static uint8_t convey_stop_bits_from_string(const std::string& p)
{
	if (!p.compare("1")) {
		return ONESTOPBIT;
	} else if (!p.compare("1.5")) {
		return ONE5STOPBITS;
	} else if (!p.compare("2")) {
		return TWOSTOPBITS;
	}
	return ((uint8_t)-1);
}

static decltype(auto) convey_get_stop_bits(std::shared_ptr<popl::Value<std::string>>& opt)
{
	uint8_t ret = ONESTOPBIT;

	if (opt->is_set()) {
		ret = convey_stop_bits_from_string(opt->value());
		if (((decltype(ret))-1) == ret) {
			std::cerr << "convey: unsupported stop bits '" << opt->value() << "'" << std::endl;
		}
	}

	return ret;
}

static convey_flow_control convey_flow_control_from_string(std::string p)
{
	for (size_t i = 0; i < p.size(); i++) {
		p[i] = std::tolower(p[i]);
	}
	if (!p.compare("none")) {
		return convey_flow_control_none;
	} else if (!p.compare("xon/xoff")) {
		return convey_flow_control_xonxoff;
	} else if (!p.compare("rts/cts")) {
		return convey_flow_control_rtscts;
	} else if (!p.compare("dsr/dtr")) {
		return convey_flow_control_dsrdtr;
	}
	return ((convey_flow_control)-1);
}

static decltype(auto) convey_get_flow_control(std::shared_ptr<popl::Value<std::string>>& opt)
{
	convey_flow_control ret = convey_flow_control_none;

	if (opt->is_set()) {
		ret = convey_flow_control_from_string(opt->value());
		if (((decltype(ret))-1) == ret) {
			std::cerr << "convey: unsupported flow control '" << opt->value() << "'" << std::endl;
		}
	}

	return ret;
}

static void convey_usage_print(popl::OptionParser& op)
{/*{{{*/
	std::cerr << "Usage: convey [options] \\\\.\\pipe\\<pipe name>" << std::endl;
	std::cerr << "       convey [options] \\\\.\\COM<num>" << std::endl;
	std::cerr << "       convey [options] tcp:<host>:<port>" << std::endl;
	std::cerr << "       convey [options] tcp-listen:<port>" << std::endl;
	std::cerr << "       convey --bridge --pipe-server \\\\.\\pipe\\<name> tcp:<host>:<port>" << std::endl;
	std::cerr << std::endl;
	std::cerr << "IPC through a named pipe, a serial port or a TCP endpoint." << std::endl;
	std::cerr << std::endl;
	std::cout << op << std::endl;
}/*}}}*/

static convey_setup_status convey_conf_setup(int argc, char **argv)
{/*{{{*/
	popl::OptionParser op{};
	auto baud_opt = op.add<popl::Value<uint32_t>>("b", "baud", "Baud rate in bps, only relevant for serial communication.", CBR_115200);
	auto parity_opt = op.add<popl::Value<std::string>>("", "parity", "Parity scheme (even, mark, no, odd, space).", "no");
	auto stop_bits_opt = op.add<popl::Value<std::string>>("", "stop-bits", "Stop bits (1, 1.5, 2).", "1");
	auto byte_size_opt = op.add<popl::Value<uint32_t>>("", "byte-size", "The number of bits in a byte.", 8);
	auto flow_control_opt = op.add<popl::Value<std::string>>("", "flow-control", "Flow control (none, xon/xoff, rts/cts, dsr/dtr).", "none");
	auto help_opt = op.add<popl::Switch>("h", "help", "Display this help message and exit.");
	auto pipe_path_opt = op.add<popl::Value<std::string>>("d", "dev", "Path to the named pipe or COM device.");
	auto pipe_poll_unavail_opt = op.add<popl::Value<double>>("p", "poll", "Poll pipe for N seconds on startup.", 0);
	auto no_xterm_opt = op.add<popl::Switch>("", "no-xterm", "Disable xterm support.");
	auto reconnect_opt = op.add<popl::Switch>("", "reconnect", "Try to reconnect after connection loss.");
	auto bridge_opt = op.add<popl::Switch>("", "bridge", "Bridge mode: pump raw bytes between a pipe server and the endpoint.");
	auto pipe_server_opt = op.add<popl::Value<std::string>>("", "pipe-server", "Create a named pipe server with this name (bridge mode).");
	auto log_opt = op.add<popl::Value<std::string>>("", "log", "Log the full session to a file, each block marked > (sent) or < (received).");
	auto log_recv_opt = op.add<popl::Value<std::string>>("", "log-recv", "Log only the received stream to a file.");
	auto log_send_opt = op.add<popl::Value<std::string>>("", "log-send", "Log only the sent stream to a file.");
	auto log_append_opt = op.add<popl::Switch>("", "log-append", "Append to the log files instead of overwriting them.");
	auto verbose_opt = op.add<popl::Switch>("v", "verbose", "Print some additional messages.");
	auto version_opt = op.add<popl::Switch>("V", "version", "Output version information and exit.");

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

		if (version_opt->count() >= 1) {
			std::cout << VERSION << std::endl;
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

		convey_transport_spec ts = convey_parse_transport(conf.pipe_path);
		if (!ts.ok) {
			if (convey_tp_tcp_server == ts.kind) {
				std::cerr << argv[0] << ": invalid listen endpoint '" << conf.pipe_path << "', expected tcp-listen:PORT" << std::endl;
			} else {
				std::cerr << argv[0] << ": invalid tcp endpoint '" << conf.pipe_path << "', expected tcp:HOST:PORT" << std::endl;
			}
			return convey_setup_exit_err;
		}
		conf.transport = ts.kind;
		conf.tcp_host = ts.host;
		conf.tcp_port = ts.port;

		if (pipe_poll_unavail_opt->is_set()) {
			conf.pipe_poll = pipe_poll_unavail_opt->value();
		}

		if (no_xterm_opt->count() >= 1) {
			conf.no_xterm = true;
		}

		conf.baud = convey_get_baud(baud_opt);
		if (((decltype(conf.baud))-1) == conf.baud) {
			return convey_setup_exit_err;
		}

		conf.parity = convey_get_parity(parity_opt);
		if (((decltype(conf.parity))-1) == conf.parity) {
			return convey_setup_exit_err;
		}

		conf.stop_bits = convey_get_stop_bits(stop_bits_opt);
		if (((decltype(conf.stop_bits))-1) == conf.stop_bits) {
			return convey_setup_exit_err;
		}

		if (byte_size_opt->is_set()) {
			conf.byte_size = byte_size_opt->value();
		} else {
			conf.byte_size = byte_size_opt->get_default();
		}

		conf.flow_control = convey_get_flow_control(flow_control_opt);

		if (reconnect_opt->count() >= 1) {
			restart_on_exit = true;
		}

		if (bridge_opt->count() >= 1) {
			conf.bridge = true;
			if (!pipe_server_opt->is_set()) {
				std::cerr << argv[0] << ": --bridge requires --pipe-server <name>" << std::endl;
				return convey_setup_exit_err;
			}
			conf.bridge_pipe_name = pipe_server_opt->value();
			restart_on_exit = true;
		}

		if (log_opt->is_set()) {
			conf.log_path = log_opt->value();
		}
		if (log_recv_opt->is_set()) {
			conf.log_recv_path = log_recv_opt->value();
		}
		if (log_send_opt->is_set()) {
			conf.log_send_path = log_send_opt->value();
		}
		if (log_append_opt->count() >= 1) {
			conf.log_append = true;
		}

		// The log options write to independent files and may be combined,
		// but two of them must not name the same file: the handles do not
		// share write access, and mixing raw and marked output would
		// corrupt the file.
		const std::string* log_paths[] = { &conf.log_path, &conf.log_recv_path, &conf.log_send_path };
		for (size_t i = 0; i < 3; ++i) {
			for (size_t j = i + 1; j < 3; ++j) {
				if (!log_paths[i]->empty() && 0 == lstrcmpiA(log_paths[i]->c_str(), log_paths[j]->c_str())) {
					std::cerr << argv[0] << ": the log options must each use a different file" << std::endl;
					return convey_setup_exit_err;
				}
			}
		}
	}
	catch (const popl::invalid_option& e)
	{
		std::cerr << "convey: " << e.what() << std::endl;
		std::cerr << "Try 'convey --help' for more information." << std::endl;
		return convey_setup_exit_err;
	}
	catch (const std::exception& e)
	{
		std::cerr << "convey: " << e.what() << std::endl;
		return convey_setup_exit_err;
	}

	return convey_setup_ok;
}/*}}}*/

static bool convey_conf_shutdown()
{/*{{{*/
	return true;
}/*}}}*/

static void convey_shutdown(void);

static void restore_console(void)
{/*{{{*/
	SetConsoleOutputCP(orig_cocp);
	SetConsoleCP(orig_ccp);
	if (!conf.no_xterm) {
		if (INVALID_HANDLE_VALUE != in) {
			SetConsoleMode(in, orig_in_cmode);
		}
		if (INVALID_HANDLE_VALUE != out) {
			SetConsoleMode(out, orig_out_cmode);
		}
	}
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

	if (!conf.no_xterm) {
		DWORD mode;

		mode = ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_INSERT_MODE | ENABLE_PROCESSED_INPUT) | ENABLE_WINDOW_INPUT;
		if (GetConsoleMode(in, &orig_in_cmode)) {
			mode &= orig_in_cmode | ENABLE_VIRTUAL_TERMINAL_INPUT;
			if (!SetConsoleMode(in, mode)) {
			mode |= ~ENABLE_VIRTUAL_TERMINAL_INPUT;
				if (!SetConsoleMode(in, mode)) {
					if (conf.verbose) {
						convey_error();
					}
				}
			}
		}

		mode = ENABLE_PROCESSED_OUTPUT;
		if (GetConsoleMode(out, &orig_out_cmode)) {
			mode |= orig_out_cmode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;
			if (!SetConsoleMode(out, mode)) {
				mode |= ~DISABLE_NEWLINE_AUTO_RETURN;
				if (!SetConsoleMode(out, mode)) {
					if (conf.verbose) {
						convey_error();
					}
				}
			}
		}
	}
}/*}}}*/

static bool convey_transport_is_tcp(void)
{/*{{{*/
	return convey_tp_tcp_client == conf.transport || convey_tp_tcp_server == conf.transport;
}/*}}}*/

static void convey_bridge_fail(void)
{/*{{{*/
	is_error = true;
	if (INVALID_HANDLE_VALUE != pipe) {
		CancelIoEx(pipe, nullptr);
	}
	if (INVALID_HANDLE_VALUE != bpipe) {
		CancelIoEx(bpipe, nullptr);
	}
}/*}}}*/

static void convey_console_fail(void)
{/*{{{*/
	is_error = true;
	/* Unblock the stdin reader so the join and any reconnect proceed at once. */
	if (INVALID_HANDLE_VALUE != stdin_thread) {
		CancelSynchronousIo(stdin_thread);
	}
	if (INVALID_HANDLE_VALUE != in) {
		CancelIoEx(in, nullptr);
	}
	if (INVALID_HANDLE_VALUE != pipe) {
		CancelIoEx(pipe, nullptr);
	}
}/*}}}*/

static bool wsa_started = false;

static bool convey_wsa_init(void)
{/*{{{*/
	if (wsa_started) {
		return true;
	}
	WSADATA wsa;
	int r = WSAStartup(MAKEWORD(2, 2), &wsa);
	if (0 != r) {
		std::cerr << "convey: WSAStartup failed with " << r << std::endl;
		return false;
	}
	wsa_started = true;
	return true;
}/*}}}*/

static void convey_final_cleanup(void)
{/*{{{*/
	if (INVALID_HANDLE_VALUE != log_handle) {
		CloseHandle(log_handle);
		log_handle = INVALID_HANDLE_VALUE;
	}
	if (INVALID_HANDLE_VALUE != log_recv_handle) {
		CloseHandle(log_recv_handle);
		log_recv_handle = INVALID_HANDLE_VALUE;
	}
	if (INVALID_HANDLE_VALUE != log_send_handle) {
		CloseHandle(log_send_handle);
		log_send_handle = INVALID_HANDLE_VALUE;
	}
	if (INVALID_SOCKET != listen_sock) {
		closesocket(listen_sock);
		listen_sock = INVALID_SOCKET;
	}
	if (wsa_started) {
		WSACleanup();
		wsa_started = false;
	}
}/*}}}*/

static void convey_log_to(HANDLE h, const char* buf, DWORD bytes)
{/*{{{*/
	if (INVALID_HANDLE_VALUE != h && bytes) {
		DWORD written = 0;
		WriteFile(h, buf, bytes, &written, nullptr);
	}
}/*}}}*/

static DWORD convey_log_session_record(char* out, const char* buf, DWORD bytes, bool sent)
{/*{{{*/
	out[0] = sent ? '>' : '<';
	out[1] = ' ';
	memcpy(out + 2, buf, bytes);
	return bytes + 2;
}/*}}}*/

static void convey_log_recv(const char* buf, DWORD bytes)
{/*{{{*/
	convey_log_to(log_recv_handle, buf, bytes);
	if (INVALID_HANDLE_VALUE != log_handle && bytes) {
		char rec[BUF_SIZE + 2];
		convey_log_to(log_handle, rec, convey_log_session_record(rec, buf, bytes, false));
	}
}/*}}}*/

static void convey_log_sent(const char* buf, DWORD bytes)
{/*{{{*/
	convey_log_to(log_send_handle, buf, bytes);
	if (INVALID_HANDLE_VALUE != log_handle && bytes) {
		char rec[BUF_SIZE + 2];
		convey_log_to(log_handle, rec, convey_log_session_record(rec, buf, bytes, true));
	}
}/*}}}*/

static bool convey_open_log(const std::string& path, HANDLE& h)
{/*{{{*/
	if (path.empty() || INVALID_HANDLE_VALUE != h) {
		return true;
	}
	DWORD disp = conf.log_append ? OPEN_ALWAYS : CREATE_ALWAYS;
	h = CreateFile(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, disp, FILE_ATTRIBUTE_NORMAL, nullptr);
	return INVALID_HANDLE_VALUE != h;
}/*}}}*/

static SOCKET convey_tcp_connect(const std::string& host, const std::string& port, DWORD& err)
{/*{{{*/
	struct addrinfo hints;
	struct addrinfo *res = nullptr;
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	int gai = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
	if (0 != gai) {
		err = static_cast<DWORD>(gai);
		return INVALID_SOCKET;
	}

	SOCKET s = INVALID_SOCKET;
	for (struct addrinfo *ai = res; nullptr != ai; ai = ai->ai_next) {
		s = WSASocketW(ai->ai_family, ai->ai_socktype, ai->ai_protocol, nullptr, 0, WSA_FLAG_OVERLAPPED);
		if (INVALID_SOCKET == s) {
			err = WSAGetLastError();
			continue;
		}
		if (0 == connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen))) {
			break;
		}
		err = WSAGetLastError();
		closesocket(s);
		s = INVALID_SOCKET;
	}
	freeaddrinfo(res);

	if (INVALID_SOCKET != s) {
		BOOL nodelay = TRUE;
		setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&nodelay), sizeof nodelay);
	}

	return s;
}/*}}}*/

static SOCKET convey_tcp_accept(const std::string& port, DWORD& err)
{/*{{{*/
	if (INVALID_SOCKET == listen_sock) {
		struct addrinfo hints;
		struct addrinfo *res = nullptr;
		memset(&hints, 0, sizeof hints);
		hints.ai_family = AF_INET6;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;
		hints.ai_flags = AI_PASSIVE;

		int gai = getaddrinfo(nullptr, port.c_str(), &hints, &res);
		if (0 != gai) {
			err = static_cast<DWORD>(gai);
			return INVALID_SOCKET;
		}

		listen_sock = WSASocketW(res->ai_family, res->ai_socktype, res->ai_protocol, nullptr, 0, WSA_FLAG_OVERLAPPED);
		if (INVALID_SOCKET == listen_sock) {
			err = WSAGetLastError();
			freeaddrinfo(res);
			return INVALID_SOCKET;
		}

		BOOL reuse = TRUE;
		setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof reuse);

		/* Accept both IPv6 and IPv4 (mapped) connections. */
		DWORD v6only = 0;
		setsockopt(listen_sock, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char *>(&v6only), sizeof v6only);

		if (0 != bind(listen_sock, res->ai_addr, static_cast<int>(res->ai_addrlen))) {
			err = WSAGetLastError();
			freeaddrinfo(res);
			closesocket(listen_sock);
			listen_sock = INVALID_SOCKET;
			return INVALID_SOCKET;
		}
		freeaddrinfo(res);

		if (0 != listen(listen_sock, 1)) {
			err = WSAGetLastError();
			closesocket(listen_sock);
			listen_sock = INVALID_SOCKET;
			return INVALID_SOCKET;
		}
	}

	SOCKET s = accept(listen_sock, nullptr, nullptr);
	if (INVALID_SOCKET == s) {
		err = WSAGetLastError();
		return INVALID_SOCKET;
	}

	BOOL nodelay = TRUE;
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&nodelay), sizeof nodelay);

	return s;
}/*}}}*/

static convey_setup_status convey_startup(int argc, char **argv)
{/*{{{*/
	DWORD rc;

	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
	_setmode(_fileno(stderr), _O_BINARY);

	convey_setup_status _rc = convey_conf_setup(argc, argv);
	if(convey_setup_ok != _rc) {
		return _rc;
	}

	if (convey_transport_is_tcp()) {
		if (!convey_wsa_init()) {
			restart_on_exit = false;
			return convey_setup_exit_err;
		}
	}

	if (conf.verbose) {
		std::cout << "Polling the pipe '" << conf.pipe_path << "' for " << conf.pipe_poll << " seconds" << std::endl;
	}
	size_t elapsed = 0,
		   step = 300 /* milliseconds*/;
	bool conn_error;
	do {
		if (convey_tp_tcp_client == conf.transport) {
			SOCKET s = convey_tcp_connect(conf.tcp_host, conf.tcp_port, rc);
			pipe = (INVALID_SOCKET == s) ? INVALID_HANDLE_VALUE : reinterpret_cast<HANDLE>(s);
			conn_error = INVALID_HANDLE_VALUE == pipe;
		} else if (convey_tp_tcp_server == conf.transport) {
			SOCKET s = convey_tcp_accept(conf.tcp_port, rc);
			pipe = (INVALID_SOCKET == s) ? INVALID_HANDLE_VALUE : reinterpret_cast<HANDLE>(s);
			conn_error = INVALID_HANDLE_VALUE == pipe;
		} else {
			pipe = CreateFile(conf.pipe_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
			rc = GetLastError();
			conn_error = INVALID_HANDLE_VALUE == pipe || ERROR_PIPE_BUSY == rc || ERROR_FILE_NOT_FOUND == rc;
		}
		if (conn_error) {
			if (elapsed/1000 < conf.pipe_poll) {
				std::this_thread::sleep_for(std::chrono::milliseconds(step));
				elapsed += step;
				continue;
			}
		}
		break;
	} while (true);
	if (conn_error) {
		convey_error(rc);
		restart_on_exit = false;
		return convey_setup_exit_err;
	}
	if (conf.verbose) {
		std::cout << "Connection established in " << (elapsed/1000) << " seconds" << std::endl;
	}

	/* TODO Expand on this handling. */
	DWORD t = GetFileType(pipe);
	if (FILE_TYPE_CHAR == t) {
		DCB dcb = {0};
		dcb.DCBlength = sizeof(DCB);

		if (!::GetCommState(pipe, &dcb)) {
			convey_error();
			return convey_setup_exit_err;
		}

		dcb.fBinary = true;
		dcb.fNull = false;
		dcb.fErrorChar = false;
		dcb.fAbortOnError = false;
		dcb.BaudRate = conf.baud;
		dcb.ByteSize = conf.byte_size;
		dcb.Parity = conf.parity;
		dcb.StopBits = conf.stop_bits;

		dcb.fOutxCtsFlow = false;
		dcb.fRtsControl = RTS_CONTROL_ENABLE;
		dcb.fOutxDsrFlow = false;
		dcb.fDtrControl = DTR_CONTROL_ENABLE;
		dcb.fTXContinueOnXoff = false;
		dcb.fOutX = false;
		dcb.fInX = false;
		dcb.fDsrSensitivity = false;
		if (convey_flow_control_rtscts == conf.flow_control) {
			dcb.fOutxCtsFlow = true;
			dcb.fRtsControl = RTS_CONTROL_HANDSHAKE;
		} else if (convey_flow_control_dsrdtr == conf.flow_control) {
			dcb.fOutxDsrFlow = true;
			dcb.fDtrControl = DTR_CONTROL_HANDSHAKE;
		} else if (convey_flow_control_xonxoff == conf.flow_control) {
			dcb.fOutX = true;
			dcb.fInX = true;
		} else if (((decltype(conf.flow_control))-1) == conf.flow_control) {
			return convey_setup_exit_err;
		}

		if (!::SetCommState(pipe, &dcb)) {
			convey_error();
			return convey_setup_exit_err;
		}

		COMMTIMEOUTS timeouts = {0};
		timeouts.ReadIntervalTimeout = MAXDWORD;
		timeouts.ReadTotalTimeoutMultiplier = 0;
		timeouts.ReadTotalTimeoutConstant = 0;
		timeouts.WriteTotalTimeoutMultiplier = 0;
		timeouts.WriteTotalTimeoutConstant = 0;
		if (!SetCommTimeouts(pipe, &timeouts)) {
			convey_error();
			return convey_setup_exit_err;
		}
	}

	if (conf.bridge) {
		bpipe = CreateNamedPipe(conf.bridge_pipe_name.c_str(),
			PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
			1, BUF_SIZE, BUF_SIZE, 0, nullptr);
		if (INVALID_HANDLE_VALUE == bpipe) {
			convey_error();
			convey_shutdown();
			return convey_setup_exit_err;
		}

		if (conf.verbose) {
			std::cout << "Waiting for a client on '" << conf.bridge_pipe_name << "'" << std::endl;
		}

		OVERLAPPED ov;
		memset(&ov, 0, sizeof ov);
		ov.hEvent = CreateEvent(nullptr, true, false, nullptr);
		BOOL connected = ConnectNamedPipe(bpipe, &ov);
		DWORD cer = GetLastError();
		if (!connected) {
			if (ERROR_IO_PENDING == cer) {
				WaitForSingleObject(ov.hEvent, INFINITE);
				connected = TRUE;
			} else if (ERROR_PIPE_CONNECTED == cer) {
				connected = TRUE;
			}
		}
		CloseHandle(ov.hEvent);
		if (!connected) {
			convey_error(cer);
			convey_shutdown();
			return convey_setup_exit_err;
		}
	} else {
		/* This could be something else, too. */
		in = GetStdHandle(STD_INPUT_HANDLE);
		if (INVALID_HANDLE_VALUE == in) {
			convey_error();
			convey_shutdown();
			return convey_setup_exit_err;
		}
		in_is_pipe = GetFileType(in) == FILE_TYPE_PIPE;

		/* This could be something else, too. */
		out = GetStdHandle(STD_OUTPUT_HANDLE);
		if (INVALID_HANDLE_VALUE == out) {
			convey_error();
			convey_shutdown();
			return convey_setup_exit_err;
		}
		out_is_pipe = GetFileType(out) == FILE_TYPE_PIPE;
	}

	e_pipe_w = CreateEvent(nullptr, false, false, nullptr);
	e_pipe_r = CreateEvent(nullptr, false, false, nullptr);
	e_in = CreateEvent(nullptr, false, false, nullptr);
	e_out = CreateEvent(nullptr, false, false, nullptr);

	if (!convey_open_log(conf.log_path, log_handle)
			|| !convey_open_log(conf.log_recv_path, log_recv_handle)
			|| !convey_open_log(conf.log_send_path, log_send_handle)) {
		convey_error();
		convey_shutdown();
		return convey_setup_exit_err;
	}

	if (!conf.bridge) {
		is_console = is_console_handle(in) && is_console_handle(out);

		if (is_console) {
			setup_console();
		}
	}

	return convey_setup_ok;
}/*}}}*/


#define CLOSE_HANDLE(h) do { \
	if (INVALID_HANDLE_VALUE != h) { \
		CloseHandle(h); \
		h = INVALID_HANDLE_VALUE; \
	} \
} while (0)
static void convey_shutdown(void)
{/*{{{*/
	shutting_down = true;

	if (is_console) {
		restore_console();
	}

	if (convey_tp_tcp_client == conf.transport || convey_tp_tcp_server == conf.transport) {
		if (INVALID_HANDLE_VALUE != pipe) {
			closesocket(reinterpret_cast<SOCKET>(pipe));
			pipe = INVALID_HANDLE_VALUE;
		}
	} else {
		CLOSE_HANDLE(pipe);
	}
	CLOSE_HANDLE(bpipe);
	/* in/out are the process standard handles; never close them, or a
	 * redirected stdio pipe would break across a --reconnect restart. */
	CLOSE_HANDLE(e_pipe_w);
	CLOSE_HANDLE(e_pipe_r);
	CLOSE_HANDLE(e_in);
	CLOSE_HANDLE(e_out);

	convey_conf_shutdown();
}
#undef CLOSE_HANDLE
/*}}}*/

#define OV_E(e) { 0, 0, {{0, 0}}, e }
static bool convey_read_pipe(HANDLE h, char (& buf)[BUF_SIZE], DWORD* bytes, HANDLE e, DWORD& er)
{
	OVERLAPPED ov = OV_E(e);
	bool rc = ReadFile(h, buf, sizeof buf, bytes, &ov);
	er = GetLastError();
	rc = convey_get_ov_result(h, &ov, bytes, rc, er);

	return rc;
}

static bool convey_write_pipe(HANDLE h, char(&buf)[BUF_SIZE], DWORD* bytes, HANDLE e, DWORD& er)
{
	DWORD total = *bytes, off = 0;

	while (off < total) {
		OVERLAPPED ov = OV_E(e);
		DWORD written = 0;
		bool rc = WriteFile(h, buf + off, total - off, &written, &ov);
		er = GetLastError();
		rc = convey_get_ov_result(h, &ov, &written, rc, er);
		if (!rc) {
			*bytes = off;
			return false;
		}
		if (0 == written) {
			break;
		}
		off += written;
	}

	*bytes = off;
	return true;
}
#undef OV_E
/* }}} */

#ifndef CONVEY_UNIT_TEST
int main(int argc, char** argv)
{/*{{{*/

	atexit(convey_final_cleanup);

restart:
	switch (convey_startup(argc, argv)) {
		case convey_setup_ok:
			// pass
			break;
		case convey_setup_exit_err:
			return 1;
		case convey_setup_exit_ok:
			return 0;
	}

	if (conf.bridge) {
		if (conf.verbose) {
			std::cout << "Bridging '" << conf.bridge_pipe_name << "' <-> '"
				<< conf.pipe_path << "'" << std::endl;
		}

		std::thread b0([]() {
			while (true) {
				if (is_error || shutting_down) {
					return;
				}

				char buf[BUF_SIZE];
				DWORD bytes{0}, er{0};

				bool rc = convey_read_pipe(pipe, buf, &bytes, e_pipe_r, er);
				if (!rc || (convey_transport_is_tcp() && 0 == bytes)) {
					if (!rc && !is_error) {
						convey_error(er);
					}
					convey_bridge_fail();
					return;
				}

				if (bytes) {
					convey_log_recv(buf, bytes);
					rc = convey_write_pipe(bpipe, buf, &bytes, e_out, er);
					if (!rc) {
						if (!is_error) {
							convey_error(er);
						}
						convey_bridge_fail();
						return;
					}
				}
			}
		});

		std::thread b1([]() {
			while (true) {
				if (is_error || shutting_down) {
					return;
				}

				char buf[BUF_SIZE];
				DWORD bytes{0}, er{0};

				bool rc = convey_read_pipe(bpipe, buf, &bytes, e_in, er);
				if (!rc) {
					if (!is_error) {
						convey_error(er);
					}
					convey_bridge_fail();
					return;
				}

				if (bytes) {
					convey_log_sent(buf, bytes);
					rc = convey_write_pipe(pipe, buf, &bytes, e_pipe_w, er);
					if (!rc) {
						if (!is_error) {
							convey_error(er);
						}
						convey_bridge_fail();
						return;
					}
				}
			}
		});

		b0.join();
		b1.join();

		convey_shutdown();

		if (restart_on_exit) {
			is_error = false;
			shutting_down = false;
			ctrl_mode = false;
			goto restart;
		}

		return 0;
	}

	std::thread t0([]() {
		while (true) {
			if (is_error || shutting_down) {
				return;
			}

			char buf[BUF_SIZE];
			DWORD bytes{0}, er{0};
			bool rc;

			if (in_is_pipe) {
				rc = convey_read_pipe(in, buf, &bytes, e_in, er);
			} else {
				rc = ReadFile(in, buf, sizeof buf, &bytes, nullptr);
				er = GetLastError();
			}
			if (!rc) {
				if (!is_error) {
					convey_error(er);
				}
				convey_console_fail();
				return;
			}

			/* Do not send bytes typed in the ctrl mode. */
			if (bytes && !ctrl_mode) {
				if (conf.no_xterm) {
					bytes = convey_trim_crlf(buf, bytes);
				}

				convey_log_sent(buf, bytes);
				rc = convey_write_pipe(pipe, buf, &bytes, e_pipe_w, er);
				if (!rc) {
					if (!is_error) {
						convey_error(er);
					}
					convey_console_fail();
					return;
				}
			} else {
				std::this_thread::sleep_for(std::chrono::milliseconds(3));
			}
		}
		return;
	});

	stdin_thread = t0.native_handle();

	std::thread t1([]() {
		while (true) {
			if (is_error || shutting_down) {
				return;
			}

			char buf[BUF_SIZE];
			DWORD bytes{0}, er{0};
			bool rc;

			rc = convey_read_pipe(pipe, buf, &bytes, e_pipe_r, er);
			if (!rc) {
				if (!is_error) {
					convey_error(er);
				}
				convey_console_fail();
				return;
			}

			if (convey_transport_is_tcp() && 0 == bytes) {
				convey_console_fail();
				return;
			}

			if (bytes) {
				convey_log_recv(buf, bytes);
				if (out_is_pipe) {
					rc = convey_write_pipe(out, buf, &bytes, e_out, er);
				} else {
					rc = WriteFile(out, buf, bytes, &bytes, nullptr);
					er = GetLastError();
				}
				if (!rc) {
					if (!is_error) {
						convey_error(er);
					}
					convey_console_fail();
					return;
				}
			} else {
				std::this_thread::sleep_for(std::chrono::milliseconds(3));
			}
		}
	});

	std::thread t2([]() {
		// Watch keystrokes to mimic screen/minicom command approach.
		// TODO yet it's simple, but integrating some curses for better control would be great.
		while (true) {
			if (is_error || shutting_down) {
				return;
			}
			if (!ctrl_mode && (GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
				ctrl_mode = true;
				if (!(GetAsyncKeyState('A') & 0x8000)) {
					ctrl_mode = false;
				}
			}
			if (ctrl_mode && (GetAsyncKeyState('Q') & 0x8000)) {
				ctrl_mode = false;
				if (conf.verbose) {
					std::cout << std::endl << "convey: ctrl-a q sent, exit" << std::endl;
				}
				convey_shutdown();
				convey_final_cleanup();
				ExitProcess(0);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(128));
		}
	});

	t0.join();
	t1.join();
	t2.join();

	convey_shutdown();

	if (restart_on_exit) {
		is_error = false;
		shutting_down = false;
		ctrl_mode = false;
		goto restart;
	}

	return 0;
}/*}}}*/
#endif

