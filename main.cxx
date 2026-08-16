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
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <afunix.h>
#include <windows.h>
#include <conio.h>
#include <fcntl.h>
#include <io.h>

#include <cstring>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <atomic>
#include <regex>
#include <vector>

#include "CLI11.hpp"

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
static std::atomic<bool> hex_on{false};
static std::atomic<bool> logging_on{true};
static std::atomic<bool> file_sent{false};
static std::atomic<bool> echo_on{false};
static std::atomic<uint64_t> bytes_rx{0};
static std::atomic<uint64_t> bytes_tx{0};
static std::atomic<uint64_t> conn_since{0};
static std::regex exit_re;
static bool have_exit = false;
static std::atomic<bool> exit_matched{false};
struct convey_expect_step { std::regex re; std::string reply; };
static std::vector<convey_expect_step> expect_steps;
static size_t expect_idx = 0;
static std::atomic<bool> expect_done{false};
static bool restart_on_exit = false;
static bool is_reconnect = false;
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
	convey_tp_tcp_server,
	convey_tp_unix_client,
	convey_tp_unix_server
};

enum convey_eol {
	convey_eol_auto,
	convey_eol_raw,
	convey_eol_lf,
	convey_eol_crlf
};

enum convey_line {
	convey_line_keep,
	convey_line_on,
	convey_line_off,
	convey_line_pulse
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
	const std::string unix_pfx = "unix:";
	const std::string unixl_pfx = "unix-listen:";

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
	} else if (0 == spec.compare(0, unixl_pfx.size(), unixl_pfx)) {
		r.kind = convey_tp_unix_server;
		r.host = spec.substr(unixl_pfx.size());
		r.ok = !r.host.empty();
	} else if (0 == spec.compare(0, unix_pfx.size(), unix_pfx)) {
		r.kind = convey_tp_unix_client;
		r.host = spec.substr(unix_pfx.size());
		r.ok = !r.host.empty();
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

/* Re-encode the outgoing newlines per the chosen policy. In auto mode the
 * historical behavior is kept: raw with xterm, trailing CRLF trimmed to LF
 * with --no-xterm. */
static std::string convey_encode_eol(const char* buf, DWORD bytes, convey_eol mode, bool no_xterm)
{/*{{{*/
	if (convey_eol_auto == mode) {
		DWORD n = no_xterm ? convey_trim_crlf(buf, bytes) : bytes;
		return std::string(buf, buf + n);
	}

	std::string out;
	out.reserve(bytes);
	if (convey_eol_crlf == mode) {
		for (DWORD i = 0; i < bytes; ++i) {
			if ('\n' == buf[i] && (0 == i || '\r' != buf[i - 1])) {
				out.push_back('\r');
			}
			out.push_back(buf[i]);
		}
	} else if (convey_eol_lf == mode) {
		for (DWORD i = 0; i < bytes; ++i) {
			if ('\r' == buf[i] && i + 1 < bytes && '\n' == buf[i + 1]) {
				continue;
			}
			out.push_back(buf[i]);
		}
	} else {
		out.assign(buf, buf + bytes);
	}
	return out;
}/*}}}*/

struct convey_conf {
	bool verbose;
	bool no_xterm;
	bool read_only;
	bool timestamps;
	bool hex;
	convey_eol eol;
	bool reconnect;
	uint32_t max_retries;
	convey_line dtr;
	convey_line rts;
	char escape;
	std::string send_file;
	uint32_t send_delay;
	std::string send_init;
	bool local_echo;
	uint32_t exit_timeout;
	uint32_t expect_timeout;
	std::string pipe_path;
	double pipe_poll;
	uint32_t baud;
	uint8_t parity;
	uint8_t stop_bits;
	uint8_t byte_size;
	uint32_t break_ms;
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

// Named rates are always valid; any other positive rate is left for the driver
// to accept or reject through SetCommState, the real authority.
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
			return b > 0;
	}
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

static convey_line convey_line_from_string(std::string p)
{
	for (size_t i = 0; i < p.size(); i++) {
		p[i] = std::tolower(p[i]);
	}
	if (!p.compare("on")) {
		return convey_line_on;
	} else if (!p.compare("off")) {
		return convey_line_off;
	} else if (!p.compare("pulse")) {
		return convey_line_pulse;
	}
	return ((convey_line)-1);
}

// Parse an escape key spec (^X, C-X, ctrl-X) into its control byte, or -1.
static int convey_parse_escape(std::string s)
{
	std::string low = s;
	for (size_t i = 0; i < low.size(); i++) {
		low[i] = std::tolower(low[i]);
	}
	char key = 0;
	if (2 == s.size() && '^' == s[0]) {
		key = s[1];
	} else if (3 == s.size() && 'c' == low[0] && ('-' == low[1] || '+' == low[1])) {
		key = s[2];
	} else if (6 == s.size() && 0 == low.compare(0, 4, "ctrl") && ('-' == low[4] || '+' == low[4])) {
		key = s[5];
	} else {
		return -1;
	}
	int up = std::toupper(static_cast<unsigned char>(key));
	if (up < '@' || up > '_') {
		return -1;
	}
	return up & 0x1f;
}

// List the serial ports present in HKLM\HARDWARE\DEVICEMAP\SERIALCOMM.
static void convey_print_serial_ports(void)
{
	HKEY key;
	if (ERROR_SUCCESS != RegOpenKeyExA(HKEY_LOCAL_MACHINE,
			"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key)) {
		std::cout << "No serial ports found." << std::endl;
		return;
	}
	bool any = false;
	for (DWORD i = 0; ; ++i) {
		char name[256];
		BYTE data[256];
		DWORD nlen = sizeof name, dlen = sizeof data, type = 0;
		if (ERROR_SUCCESS != RegEnumValueA(key, i, name, &nlen, nullptr, &type, data, &dlen)) {
			break;
		}
		if (REG_SZ == type) {
			std::cout << reinterpret_cast<const char*>(data) << std::endl;
			any = true;
		}
	}
	RegCloseKey(key);
	if (!any) {
		std::cout << "No serial ports found." << std::endl;
	}
}

// Decode backslash escapes (\r \n \t \0 \\ \xHH) into raw bytes; false on a
// malformed \x.
static bool convey_unescape(const std::string& in, std::string& out)
{
	for (size_t i = 0; i < in.size(); ++i) {
		if ('\\' != in[i]) {
			out.push_back(in[i]);
			continue;
		}
		if (++i >= in.size()) {
			out.push_back('\\');
			break;
		}
		switch (in[i]) {
		case 'r': out.push_back('\r'); break;
		case 'n': out.push_back('\n'); break;
		case 't': out.push_back('\t'); break;
		case '0': out.push_back('\0'); break;
		case '\\': out.push_back('\\'); break;
		case 'x': {
			if (i + 2 >= in.size()) {
				return false;
			}
			auto hex = [](char h) -> int {
				if (h >= '0' && h <= '9') return h - '0';
				if (h >= 'a' && h <= 'f') return h - 'a' + 10;
				if (h >= 'A' && h <= 'F') return h - 'A' + 10;
				return -1;
			};
			int hi = hex(in[i + 1]), lo = hex(in[i + 2]);
			if (hi < 0 || lo < 0) {
				return false;
			}
			out.push_back(static_cast<char>((hi << 4) | lo));
			i += 2;
			break;
		}
		default: out.push_back(in[i]); break;
		}
	}
	return true;
}

// Single source for the session key list, shared by --help-keys and the runtime menu.
static void convey_key_help(std::ostream &out, const char *nl)
{
	out << "Session commands. The escape key is Ctrl+A by default." << nl
		<< "Press the escape key then one of these keys." << nl
		<< "  q   quit" << nl
		<< "  b   send a serial BREAK" << nl
		<< "  r   pulse the serial DTR line to reset" << nl
		<< "  m   show the modem line status" << nl
		<< "  h   toggle the hex view" << nl
		<< "  e   toggle local echo" << nl
		<< "  l   toggle logging" << nl
		<< "  c   clear the screen" << nl
		<< "  x   send a file" << nl
		<< "  .   show connection status" << nl
		<< "  ?   show this list" << nl;
}

static convey_setup_status convey_conf_setup(int argc, char **argv)
{/*{{{*/
	CLI::App app{"IPC through a named pipe, a serial port or a TCP endpoint.", "convey"};
	app.usage(
		"Usage: convey [options] \\\\.\\pipe\\<pipe name>\n"
		"       convey [options] \\\\.\\COM<num>\n"
		"       convey [options] tcp:<host>:<port>\n"
		"       convey [options] tcp-listen:<port>\n"
		"       convey [options] unix:<path>\n"
		"       convey [options] unix-listen:<path>\n"
		"       convey --bridge --pipe-server \\\\.\\pipe\\<name> tcp:<host>:<port>");
	app.get_formatter()->column_width(34);
	app.get_formatter()->long_option_alignment_ratio(6.f / 34.f);
	app.get_formatter()->enable_footer_formatting(false);

	std::string target;
	std::string dev;
	std::string log_path, log_recv_path, log_send_path, pipe_server;
	std::string parity = "no", stop_bits = "1", flow_control = "none";
	std::string dtr_arg, rts_arg;
	uint32_t baud = CBR_115200, byte_size = 8;
	uint32_t break_ms = 300;
	double poll = 0.0;
	bool bridge = false, reconnect = false, no_xterm = false, read_only = false, timestamps = false, hex = false, log_append = false, verbose = false;
	bool eol_raw = false, eol_lf = false, eol_crlf = false;
	bool list_ports = false;
	bool help_keys = false;
	uint32_t max_retries = 0;
	std::string escape_arg;
	std::string send_file;
	uint32_t send_delay = 0;
	std::string send_init_arg;
	bool local_echo = false;
	std::string exit_arg;
	uint32_t exit_timeout = 0;
	std::vector<std::string> expect_pats, expect_replies;
	uint32_t expect_timeout = 0;

	// Endpoint given as the first positional argument; --dev is an alias.
	app.add_option("target", target, "")->group("");

	app.add_option("-d,--dev", dev, "Path to the named pipe or COM device.")->group("Connection")->type_name("PATH");
	app.add_option("-p,--poll", poll, "Poll pipe for N seconds on startup.")->group("Connection")->capture_default_str()->type_name("SECONDS");
	app.add_flag("--reconnect", reconnect, "Try to reconnect after connection loss.")->group("Connection");
	app.add_option("--max-retries", max_retries, "Give up reconnecting after N attempts (0 = unlimited).")->group("Connection")->capture_default_str()->type_name("N");
	app.add_flag("--list-serial-ports", list_ports, "List available serial ports and exit.")->group("Connection");
	app.add_option("--send-file", send_file, "Stream a file to the target after connecting.")->group("Connection")->type_name("FILE");
	app.add_option("--send-delay", send_delay, "Delay in ms between sent lines when sending a file.")->group("Connection")->capture_default_str()->type_name("MS");
	app.add_option("--send-init", send_init_arg, "Send this string after each connect (\\r, \\n, \\xHH escapes).")->group("Connection")->type_name("STR");
	app.add_option("--exit-on", exit_arg, "Exit once this regex appears in the received stream.")->group("Connection")->type_name("REGEX");
	app.add_option("--exit-timeout", exit_timeout, "Exit with an error if --exit-on is not seen in N seconds (0 = wait forever).")->group("Connection")->capture_default_str()->type_name("SEC");
	app.add_option("--expect", expect_pats, "Wait for this regex, then send the matching --reply (repeatable).")->group("Connection")->type_name("REGEX")->allow_extra_args(false);
	app.add_option("--reply", expect_replies, "Reply to send when the matching --expect is seen (escapes allowed).")->group("Connection")->type_name("STR")->allow_extra_args(false);
	app.add_option("--expect-timeout", expect_timeout, "Fail if the --expect sequence is not completed in N seconds.")->group("Connection")->capture_default_str()->type_name("SEC");

	app.add_option("-b,--baud", baud, "Baud rate in bps for serial, a standard or custom rate the device supports.")->group("Serial")->capture_default_str()->type_name("RATE");
	app.add_option("--byte-size", byte_size, "The number of bits in a byte.")->group("Serial")->capture_default_str()->type_name("BITS");
	app.add_option("--parity", parity, "Parity scheme (even, mark, no, odd, space).")->group("Serial")->capture_default_str()->type_name("SCHEME");
	app.add_option("--stop-bits", stop_bits, "Stop bits (1, 1.5, 2).")->group("Serial")->capture_default_str()->type_name("BITS");
	app.add_option("--break-ms", break_ms, "How long the ctrl-a b serial BREAK is held, in ms.")->group("Serial")->capture_default_str()->type_name("MS");
	app.add_option("--flow-control", flow_control, "Flow control (none, xon/xoff, rts/cts, dsr/dtr).")->group("Serial")->capture_default_str()->type_name("MODE");
	app.add_option("--dtr", dtr_arg, "Set the serial DTR line (on, off, pulse).")->group("Serial")->type_name("STATE");
	app.add_option("--rts", rts_arg, "Set the serial RTS line (on, off, pulse).")->group("Serial")->type_name("STATE");

	app.add_flag("--bridge", bridge, "Bridge mode: pump raw bytes between a pipe server and the endpoint.")->group("Bridge");
	app.add_option("--pipe-server", pipe_server, "Create a named pipe server with this name (bridge mode).")->group("Bridge")->type_name("NAME");

	app.add_option("--log", log_path, "Log the full session to a file, each block marked > (sent) or < (received).")->group("Logging")->type_name("FILE");
	app.add_option("--log-recv", log_recv_path, "Log only the received stream to a file.")->group("Logging")->type_name("FILE");
	app.add_option("--log-send", log_send_path, "Log only the sent stream to a file.")->group("Logging")->type_name("FILE");
	app.add_flag("--log-append", log_append, "Append to the log files instead of overwriting them.")->group("Logging");

	app.add_flag("--no-xterm", no_xterm, "Disable xterm support.")->group("General");
	app.add_flag("--read-only", read_only, "Monitor only and do not send anything to the endpoint.")->group("General");
	app.add_flag("--timestamps", timestamps, "Prefix each received line with a local time stamp.")->group("General");
	app.add_flag("--hex", hex, "Show the received stream as a hex dump instead of text.")->group("General");
	app.add_flag("--local-echo", local_echo, "Echo typed input locally for non-echoing targets.")->group("General");
	app.add_option("--escape", escape_arg, "Escape key for the ctrl-a commands (^A, ctrl-], ...).")->group("General")->type_name("KEY");
	app.add_flag("--raw", eol_raw, "Send input with no newline translation. Excludes --lf, --crlf.")->group("General");
	app.add_flag("--lf", eol_lf, "Translate sent CRLF newlines to LF. Excludes --raw, --crlf.")->group("General");
	app.add_flag("--crlf", eol_crlf, "Translate sent LF newlines to CRLF. Excludes --raw, --lf.")->group("General");
	app.set_help_flag("-h,--help", "Display this help message and exit.")->group("General");
	app.add_flag("--help-keys", help_keys, "List the runtime session key commands and exit.")->group("General");
	app.set_version_flag("-V,--version", std::string(VERSION), "Output version information and exit.")->group("General");
	app.add_flag("-v,--verbose", verbose, "Print some additional messages.")->group("General");

	app.footer(
		"Session commands are available at runtime. Press Ctrl+A then ? for the list,\n"
		"or run convey --help-keys.");

	try {
		app.parse(argc, argv);
	}
	catch (const CLI::CallForHelp&) {
		std::cout << app.help();
		return convey_setup_exit_ok;
	}
	catch (const CLI::CallForVersion&) {
		std::cout << VERSION << std::endl;
		return convey_setup_exit_ok;
	}
	catch (const CLI::ParseError& e) {
		std::cerr << "convey: " << e.what() << std::endl;
		std::cerr << "Try 'convey --help' for more information." << std::endl;
		return convey_setup_exit_err;
	}

	if (help_keys) {
		convey_key_help(std::cout, "\n");
		return convey_setup_exit_ok;
	}

	if (list_ports) {
		convey_print_serial_ports();
		return convey_setup_exit_ok;
	}

	conf.verbose = verbose;

	if (!dev.empty()) {
		conf.pipe_path = dev;
	} else if (!target.empty()) {
		conf.pipe_path = target;
	} else {
		std::cerr << argv[0] << ": empty pipe path" << std::endl;
		std::cerr << "Try 'convey --help' for more information." << std::endl;
		return convey_setup_exit_err;
	}

	convey_transport_spec ts = convey_parse_transport(conf.pipe_path);
	if (!ts.ok) {
		if (convey_tp_tcp_server == ts.kind) {
			std::cerr << argv[0] << ": invalid listen endpoint '" << conf.pipe_path << "', expected tcp-listen:PORT" << std::endl;
		} else if (convey_tp_unix_server == ts.kind) {
			std::cerr << argv[0] << ": invalid listen endpoint '" << conf.pipe_path << "', expected unix-listen:PATH" << std::endl;
		} else if (convey_tp_unix_client == ts.kind) {
			std::cerr << argv[0] << ": invalid unix endpoint '" << conf.pipe_path << "', expected unix:PATH" << std::endl;
		} else {
			std::cerr << argv[0] << ": invalid tcp endpoint '" << conf.pipe_path << "', expected tcp:HOST:PORT" << std::endl;
		}
		return convey_setup_exit_err;
	}
	conf.transport = ts.kind;
	conf.tcp_host = ts.host;
	conf.tcp_port = ts.port;

	conf.pipe_poll = poll;
	conf.no_xterm = no_xterm;
	conf.read_only = read_only;
	conf.timestamps = timestamps;
	conf.hex = hex;
	hex_on = hex;
	if ((eol_raw ? 1 : 0) + (eol_lf ? 1 : 0) + (eol_crlf ? 1 : 0) > 1) {
		std::cerr << "convey: --raw, --lf and --crlf are mutually exclusive" << std::endl;
		return convey_setup_exit_err;
	}
	conf.eol = eol_crlf ? convey_eol_crlf : eol_lf ? convey_eol_lf : eol_raw ? convey_eol_raw : convey_eol_auto;
	conf.reconnect = reconnect;
	conf.max_retries = max_retries;

	conf.escape = 0x01;
	if (!escape_arg.empty()) {
		int e = convey_parse_escape(escape_arg);
		if (e < 0) {
			std::cerr << "convey: unsupported escape key '" << escape_arg << "'" << std::endl;
			return convey_setup_exit_err;
		}
		conf.escape = static_cast<char>(e);
	}

	conf.send_file = send_file;
	conf.send_delay = send_delay;
	conf.local_echo = local_echo;
	echo_on = local_echo;

	conf.send_init.clear();
	if (!send_init_arg.empty() && !convey_unescape(send_init_arg, conf.send_init)) {
		std::cerr << "convey: invalid --send-init escape in '" << send_init_arg << "'" << std::endl;
		return convey_setup_exit_err;
	}

	have_exit = false;
	exit_matched = false;
	if (!exit_arg.empty()) {
		try {
			exit_re.assign(exit_arg, std::regex::ECMAScript);
			have_exit = true;
		} catch (const std::regex_error&) {
			std::cerr << "convey: invalid --exit-on regex '" << exit_arg << "'" << std::endl;
			return convey_setup_exit_err;
		}
	}
	conf.exit_timeout = exit_timeout;

	expect_steps.clear();
	expect_idx = 0;
	expect_done = false;
	if (expect_pats.size() != expect_replies.size()) {
		std::cerr << "convey: each --expect needs a matching --reply" << std::endl;
		return convey_setup_exit_err;
	}
	for (size_t i = 0; i < expect_pats.size(); ++i) {
		convey_expect_step step;
		try {
			step.re.assign(expect_pats[i], std::regex::ECMAScript);
		} catch (const std::regex_error&) {
			std::cerr << "convey: invalid --expect regex '" << expect_pats[i] << "'" << std::endl;
			return convey_setup_exit_err;
		}
		if (!convey_unescape(expect_replies[i], step.reply)) {
			std::cerr << "convey: invalid --reply escape in '" << expect_replies[i] << "'" << std::endl;
			return convey_setup_exit_err;
		}
		expect_steps.push_back(std::move(step));
	}
	conf.expect_timeout = expect_timeout;

	if (!convey_baud_is_valid(baud)) {
		std::cerr << "convey: unsupported baud rate '" << baud << "'" << std::endl;
		return convey_setup_exit_err;
	}
	conf.baud = baud;
	conf.break_ms = break_ms;

	uint8_t par = convey_parity_from_string(parity);
	if (((uint8_t)-1) == par) {
		std::cerr << "convey: unsupported parity '" << parity << "'" << std::endl;
		return convey_setup_exit_err;
	}
	conf.parity = par;

	uint8_t sb = convey_stop_bits_from_string(stop_bits);
	if (((uint8_t)-1) == sb) {
		std::cerr << "convey: unsupported stop bits '" << stop_bits << "'" << std::endl;
		return convey_setup_exit_err;
	}
	conf.stop_bits = sb;

	convey_flow_control fc = convey_flow_control_from_string(flow_control);
	if (((convey_flow_control)-1) == fc) {
		std::cerr << "convey: unsupported flow control '" << flow_control << "'" << std::endl;
		return convey_setup_exit_err;
	}
	conf.flow_control = fc;

	conf.byte_size = byte_size;

	conf.dtr = convey_line_keep;
	if (!dtr_arg.empty()) {
		conf.dtr = convey_line_from_string(dtr_arg);
		if (((convey_line)-1) == conf.dtr) {
			std::cerr << "convey: unsupported dtr state '" << dtr_arg << "'" << std::endl;
			return convey_setup_exit_err;
		}
	}
	conf.rts = convey_line_keep;
	if (!rts_arg.empty()) {
		conf.rts = convey_line_from_string(rts_arg);
		if (((convey_line)-1) == conf.rts) {
			std::cerr << "convey: unsupported rts state '" << rts_arg << "'" << std::endl;
			return convey_setup_exit_err;
		}
	}

	if (reconnect) {
		restart_on_exit = true;
	}

	if (bridge) {
		conf.bridge = true;
		if (pipe_server.empty()) {
			std::cerr << argv[0] << ": --bridge requires --pipe-server <name>" << std::endl;
			return convey_setup_exit_err;
		}
		conf.bridge_pipe_name = pipe_server;
		restart_on_exit = true;
	}

	conf.log_path = log_path;
	conf.log_recv_path = log_recv_path;
	conf.log_send_path = log_send_path;
	conf.log_append = log_append;

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

static bool convey_transport_is_unix(void)
{/*{{{*/
	return convey_tp_unix_client == conf.transport || convey_tp_unix_server == conf.transport;
}/*}}}*/

static bool convey_transport_is_socket(void)
{/*{{{*/
	return convey_transport_is_tcp() || convey_transport_is_unix();
}/*}}}*/

// A socket read or write failure is a disconnect, quiet unless verbose.
static bool convey_report_io_error(void)
{/*{{{*/
	return conf.verbose || !convey_transport_is_socket();
}/*}}}*/

// Compact elapsed time, seconds under a minute, else with minutes and hours.
static std::string convey_fmt_uptime(uint64_t ms)
{/*{{{*/
	unsigned long s = static_cast<unsigned long>(ms / 1000);
	unsigned long h = s / 3600, m = (s % 3600) / 60, sec = s % 60;
	char b[48];
	if (h) {
		wsprintfA(b, "%luh %02lum %02lus", h, m, sec);
	} else if (m) {
		wsprintfA(b, "%lum %02lus", m, sec);
	} else {
		wsprintfA(b, "%lus", sec);
	}
	return b;
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
	if (convey_tp_unix_server == conf.transport && !conf.tcp_host.empty()) {
		DeleteFileA(conf.tcp_host.c_str());
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
	if (!logging_on) {
		return;
	}
	convey_log_to(log_recv_handle, buf, bytes);
	if (INVALID_HANDLE_VALUE != log_handle && bytes) {
		char rec[BUF_SIZE + 2];
		convey_log_to(log_handle, rec, convey_log_session_record(rec, buf, bytes, false));
	}
}/*}}}*/

static void convey_log_sent(const char* buf, DWORD bytes)
{/*{{{*/
	if (!logging_on) {
		return;
	}
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

/* Low latency plus dead-peer detection for a serial-over-TCP data socket. */
static void convey_tcp_tune(SOCKET s)
{/*{{{*/
	BOOL nodelay = TRUE;
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&nodelay), sizeof nodelay);

	BOOL keepalive = TRUE;
	setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char *>(&keepalive), sizeof keepalive);

	/* Probe after 30 s idle, then every 5 s, so a dropped peer is seen in
	 * about a minute instead of the two-hour default. */
	struct tcp_keepalive ka;
	ka.onoff = 1;
	ka.keepalivetime = 30000;
	ka.keepaliveinterval = 5000;
	DWORD ret = 0;
	WSAIoctl(s, SIO_KEEPALIVE_VALS, &ka, sizeof ka, nullptr, 0, &ret, nullptr, nullptr);
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
		convey_tcp_tune(s);
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

	convey_tcp_tune(s);

	return s;
}/*}}}*/

static bool convey_unix_set_addr(struct sockaddr_un& addr, const std::string& path, DWORD& err)
{/*{{{*/
	if (path.size() >= sizeof addr.sun_path) {
		err = ERROR_BUFFER_OVERFLOW;
		return false;
	}
	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path, path.c_str(), path.size());
	return true;
}/*}}}*/

static SOCKET convey_unix_connect(const std::string& path, DWORD& err)
{/*{{{*/
	struct sockaddr_un addr;
	if (!convey_unix_set_addr(addr, path, err)) {
		return INVALID_SOCKET;
	}

	SOCKET s = WSASocketW(AF_UNIX, SOCK_STREAM, 0, nullptr, 0, WSA_FLAG_OVERLAPPED);
	if (INVALID_SOCKET == s) {
		err = WSAGetLastError();
		return INVALID_SOCKET;
	}
	if (0 != connect(s, reinterpret_cast<struct sockaddr *>(&addr), sizeof addr)) {
		err = WSAGetLastError();
		closesocket(s);
		return INVALID_SOCKET;
	}

	return s;
}/*}}}*/

static SOCKET convey_unix_accept(const std::string& path, DWORD& err)
{/*{{{*/
	if (INVALID_SOCKET == listen_sock) {
		struct sockaddr_un addr;
		if (!convey_unix_set_addr(addr, path, err)) {
			return INVALID_SOCKET;
		}

		/* A stale socket file blocks bind, so remove it first. */
		DeleteFileA(path.c_str());

		listen_sock = WSASocketW(AF_UNIX, SOCK_STREAM, 0, nullptr, 0, WSA_FLAG_OVERLAPPED);
		if (INVALID_SOCKET == listen_sock) {
			err = WSAGetLastError();
			return INVALID_SOCKET;
		}
		if (0 != bind(listen_sock, reinterpret_cast<struct sockaddr *>(&addr), sizeof addr)) {
			err = WSAGetLastError();
			closesocket(listen_sock);
			listen_sock = INVALID_SOCKET;
			return INVALID_SOCKET;
		}
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

	return s;
}/*}}}*/

// Next backoff delay in ms: double the step, clamped to the cap.
static size_t convey_backoff_next(size_t step, size_t cap)
{/*{{{*/
	size_t n = step * 2;
	return n > cap ? cap : n;
}/*}}}*/

static void convey_set_line(HANDLE h, bool dtr, bool on)
{/*{{{*/
	EscapeCommFunction(h, dtr ? (on ? SETDTR : CLRDTR) : (on ? SETRTS : CLRRTS));
}/*}}}*/

// Apply a DTR/RTS action; pulse briefly de-asserts the line then re-asserts it.
static void convey_apply_line(HANDLE h, bool dtr, convey_line action)
{/*{{{*/
	switch (action) {
	case convey_line_on:
		convey_set_line(h, dtr, true);
		break;
	case convey_line_off:
		convey_set_line(h, dtr, false);
		break;
	case convey_line_pulse:
		convey_set_line(h, dtr, false);
		std::this_thread::sleep_for(std::chrono::milliseconds(120));
		convey_set_line(h, dtr, true);
		break;
	default:
		break;
	}
}/*}}}*/

static convey_setup_status convey_connect(int argc, char **argv)
{/*{{{*/
	DWORD rc;

	if (convey_transport_is_socket()) {
		if (!convey_wsa_init()) {
			restart_on_exit = false;
			return convey_setup_exit_err;
		}
	}

	if (conf.verbose) {
		std::cout << "Polling the pipe '" << conf.pipe_path << "' for " << conf.pipe_poll << " seconds" << std::endl;
	}
	size_t elapsed = 0,
		   step = 300 /* milliseconds*/,
		   step_cap = 5000,
		   attempts = 0;
	bool conn_error, announced = false;
	do {
		if (convey_tp_tcp_client == conf.transport) {
			SOCKET s = convey_tcp_connect(conf.tcp_host, conf.tcp_port, rc);
			pipe = (INVALID_SOCKET == s) ? INVALID_HANDLE_VALUE : reinterpret_cast<HANDLE>(s);
			conn_error = INVALID_HANDLE_VALUE == pipe;
		} else if (convey_tp_tcp_server == conf.transport) {
			SOCKET s = convey_tcp_accept(conf.tcp_port, rc);
			pipe = (INVALID_SOCKET == s) ? INVALID_HANDLE_VALUE : reinterpret_cast<HANDLE>(s);
			conn_error = INVALID_HANDLE_VALUE == pipe;
		} else if (convey_tp_unix_client == conf.transport) {
			SOCKET s = convey_unix_connect(conf.tcp_host, rc);
			pipe = (INVALID_SOCKET == s) ? INVALID_HANDLE_VALUE : reinterpret_cast<HANDLE>(s);
			conn_error = INVALID_HANDLE_VALUE == pipe;
		} else if (convey_tp_unix_server == conf.transport) {
			SOCKET s = convey_unix_accept(conf.tcp_host, rc);
			pipe = (INVALID_SOCKET == s) ? INVALID_HANDLE_VALUE : reinterpret_cast<HANDLE>(s);
			conn_error = INVALID_HANDLE_VALUE == pipe;
		} else {
			pipe = CreateFile(conf.pipe_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
			rc = GetLastError();
			conn_error = INVALID_HANDLE_VALUE == pipe || ERROR_PIPE_BUSY == rc || ERROR_FILE_NOT_FOUND == rc;
		}
		if (!conn_error || shutting_down) {
			break;
		}

		/* On a post-drop reconnect keep retrying with exponential backoff,
		 * bounded by --max-retries; the initial connect stays within the
		 * --poll window. */
		bool retry;
		if (is_reconnect && conf.reconnect) {
			++attempts;
			retry = 0 == conf.max_retries || attempts < conf.max_retries;
		} else {
			retry = elapsed/1000 < conf.pipe_poll;
		}
		if (!retry) {
			break;
		}
		if (is_reconnect && !announced) {
			std::cerr << "convey: reconnecting...\r\n";
			announced = true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(step));
		elapsed += step;
		step = convey_backoff_next(step, step_cap);
	} while (true);
	if (conn_error) {
		convey_error(rc);
		restart_on_exit = false;
		return convey_setup_exit_err;
	}
	if (is_reconnect) {
		std::cerr << "convey: reconnected\r\n";
	}
	if (conf.verbose) {
		std::cout << "Connection established in " << (elapsed/1000) << " seconds" << std::endl;
	}
	conn_since = GetTickCount64();

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

		if (convey_line_keep != conf.dtr) {
			convey_apply_line(pipe, true, conf.dtr);
		}
		if (convey_line_keep != conf.rts) {
			convey_apply_line(pipe, false, conf.rts);
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

	if (convey_transport_is_socket()) {
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

	if (rc && h == pipe && *bytes) {
		bytes_rx += *bytes;
	}

	return rc;
}

static bool convey_write_pipe(HANDLE h, const char* buf, DWORD* bytes, HANDLE e, DWORD& er)
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
			if (h == pipe && off) {
				bytes_tx += off;
			}
			return false;
		}
		if (0 == written) {
			break;
		}
		off += written;
	}

	*bytes = off;
	if (h == pipe && off) {
		bytes_tx += off;
	}
	return true;
}

static void convey_stamp_lines(const char* buf, DWORD bytes, bool& at_line_start, std::string& out)
{/*{{{*/
	for (DWORD i = 0; i < bytes; ++i) {
		if (at_line_start) {
			SYSTEMTIME st;
			GetLocalTime(&st);
			char stamp[16];
			int n = wsprintfA(stamp, "[%02u:%02u:%02u] ", st.wHour, st.wMinute, st.wSecond);
			out.append(stamp, n);
			at_line_start = false;
		}
		out.push_back(buf[i]);
		if ('\n' == buf[i]) {
			at_line_start = true;
		}
	}
}/*}}}*/

static void convey_hexdump(const char* buf, DWORD bytes, size_t& offset, std::string& out)
{/*{{{*/
	DWORD i = 0;
	while (i < bytes) {
		DWORD row = (bytes - i < 16) ? (bytes - i) : 16;
		char head[16];
		int n = wsprintfA(head, "%08lx  ", (unsigned long)offset);
		out.append(head, n);
		for (DWORD j = 0; j < 16; ++j) {
			if (j < row) {
				char hb[8];
				int m = wsprintfA(hb, "%02x ", (unsigned char)buf[i + j]);
				out.append(hb, m);
			} else {
				out.append("   ", 3);
			}
			if (7 == j) {
				out.push_back(' ');
			}
		}
		out.append("|", 1);
		for (DWORD j = 0; j < row; ++j) {
			unsigned char c = (unsigned char)buf[i + j];
			out.push_back((c >= 0x20 && c < 0x7f) ? (char)c : '.');
		}
		out.append("|\n", 2);
		offset += row;
		i += row;
	}
}/*}}}*/

static DWORD convey_filter_escape(char* buf, DWORD bytes, bool& esc_pending, std::string& cmds, char esc)
{/*{{{*/
	DWORD out = 0;
	for (DWORD i = 0; i < bytes; ++i) {
		char c = buf[i];
		if (esc_pending) {
			esc_pending = false;
			cmds.push_back(c);
		} else if (esc == c) {
			esc_pending = true;
		} else {
			buf[out++] = c;
		}
	}
	return out;
}/*}}}*/
#undef OV_E
/* }}} */

#ifndef CONVEY_UNIT_TEST
static bool convey_read_console(HANDLE h, char* buf, DWORD cap, DWORD* bytes)
{/*{{{*/
	INPUT_RECORD recs[64];
	DWORD n = 0;
	if (!ReadConsoleInputW(h, recs, 64, &n)) {
		return false;
	}
	DWORD out = 0;
	for (DWORD i = 0; i < n; ++i) {
		if (KEY_EVENT != recs[i].EventType || !recs[i].Event.KeyEvent.bKeyDown) {
			continue;
		}
		wchar_t wc = recs[i].Event.KeyEvent.uChar.UnicodeChar;
		if (0 == wc) {
			continue;
		}
		for (WORD r = 0; r < recs[i].Event.KeyEvent.wRepeatCount; ++r) {
			char mb[8];
			int mlen = WideCharToMultiByte(CP_UTF8, 0, &wc, 1, mb, sizeof mb, nullptr, nullptr);
			for (int k = 0; k < mlen && out < cap; ++k) {
				buf[out++] = mb[k];
			}
		}
	}
	*bytes = out;
	return true;
}/*}}}*/

// Stream a file to the target, optionally pacing per line; console-side only.
static bool convey_send_file(const std::string& path)
{/*{{{*/
	if (INVALID_HANDLE_VALUE == pipe) {
		std::cerr << "\r\nconvey: not connected\r\n";
		return false;
	}
	HANDLE fh = CreateFile(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (INVALID_HANDLE_VALUE == fh) {
		std::cerr << "\r\nconvey: cannot open '" << path << "'\r\n";
		return false;
	}
	std::string data;
	char rb[BUF_SIZE];
	DWORD rd = 0;
	while (ReadFile(fh, rb, sizeof rb, &rd, nullptr) && rd) {
		data.append(rb, rd);
	}
	CloseHandle(fh);

	std::string enc = convey_encode_eol(data.data(), static_cast<DWORD>(data.size()), conf.eol, conf.no_xterm);

	HANDLE ev = CreateEvent(nullptr, TRUE, FALSE, nullptr);
	size_t off = 0, total = enc.size();
	DWORD er = 0;
	bool ok = true;
	while (off < total) {
		size_t end;
		if (conf.send_delay > 0) {
			size_t nl = enc.find('\n', off);
			end = (std::string::npos == nl) ? total : nl + 1;
		} else {
			end = off + BUF_SIZE;
			if (end > total) {
				end = total;
			}
		}
		DWORD n = static_cast<DWORD>(end - off);
		if (!convey_write_pipe(pipe, enc.data() + off, &n, ev, er)) {
			ok = false;
			break;
		}
		off += n;
		if (conf.send_delay > 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(conf.send_delay));
		}
	}
	CloseHandle(ev);

	if (ok) {
		std::cerr << "\r\nconvey: sent '" << path << "' (" << off << " bytes)\r\n";
	} else {
		std::cerr << "\r\nconvey: send failed for '" << path << "'\r\n";
	}
	return ok;
}/*}}}*/

// Read one line from the console in cooked mode; used for interactive prompts.
static bool convey_prompt_line(const char* prompt, std::string& out)
{/*{{{*/
	std::cout << "\r\n" << prompt << std::flush;
	DWORD saved = 0;
	bool have = 0 != GetConsoleMode(in, &saved);
	if (have) {
		SetConsoleMode(in, ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
	}
	wchar_t wbuf[1024];
	DWORD n = 0;
	bool ok = 0 != ReadConsoleW(in, wbuf, 1023, &n, nullptr);
	if (have) {
		SetConsoleMode(in, saved);
	}
	if (!ok) {
		return false;
	}
	while (n > 0 && (L'\n' == wbuf[n - 1] || L'\r' == wbuf[n - 1])) {
		--n;
	}
	char mb[2048];
	int mlen = WideCharToMultiByte(CP_UTF8, 0, wbuf, static_cast<int>(n), mb, sizeof mb, nullptr, nullptr);
	out.assign(mb, mlen > 0 ? mlen : 0);
	return !out.empty();
}/*}}}*/

// Echo typed input to stdout, expanding CR to CRLF so lines advance.
static void convey_echo_local(const char* buf, DWORD bytes)
{/*{{{*/
	if (!echo_on) {
		return;
	}
	std::string e;
	e.reserve(bytes + 8);
	for (DWORD i = 0; i < bytes; ++i) {
		if ('\r' == buf[i]) {
			e.append("\r\n", 2);
		} else {
			e.push_back(buf[i]);
		}
	}
	DWORD w = 0;
	WriteFile(out, e.data(), static_cast<DWORD>(e.size()), &w, nullptr);
}/*}}}*/

static void convey_quit(int code)
{/*{{{*/
	restart_on_exit = false;
	convey_shutdown();
	convey_final_cleanup();
	ExitProcess(static_cast<UINT>(code));
}/*}}}*/

// Feed received bytes to the --exit-on matcher; exit 0 on the first match.
static void convey_exit_feed(const char* buf, DWORD bytes)
{/*{{{*/
	if (!have_exit || exit_matched) {
		return;
	}
	static std::string accum;
	accum.append(buf, bytes);
	if (std::regex_search(accum, exit_re)) {
		exit_matched = true;
		std::cerr << "\r\nconvey: --exit-on matched\r\n";
		convey_quit(0);
	}
	if (accum.size() > 65536) {
		accum.erase(0, accum.size() - 4096);
	}
}/*}}}*/

// Drive the --expect/--reply sequence: on each step's match send its reply and
// advance; the session continues after the last step.
static void convey_expect_feed(const char* buf, DWORD bytes)
{/*{{{*/
	if (expect_steps.empty() || expect_idx >= expect_steps.size()) {
		return;
	}
	static std::string accum;
	accum.append(buf, bytes);
	if (std::regex_search(accum, expect_steps[expect_idx].re)) {
		const std::string& reply = expect_steps[expect_idx].reply;
		if (!reply.empty() && INVALID_HANDLE_VALUE != pipe) {
			HANDLE ev = CreateEvent(nullptr, TRUE, FALSE, nullptr);
			DWORD n = static_cast<DWORD>(reply.size()), er = 0;
			convey_write_pipe(pipe, reply.data(), &n, ev, er);
			CloseHandle(ev);
		}
		++expect_idx;
		accum.clear();
		if (expect_idx >= expect_steps.size()) {
			expect_done = true;
			std::cerr << "\r\nconvey: expect sequence complete\r\n";
		}
	}
	if (accum.size() > 65536) {
		accum.erase(0, accum.size() - 4096);
	}
}/*}}}*/
/* Turn a bare Ctrl+C/Ctrl+Break/console-close into an orderly shutdown so the
 * console mode is restored and the transport and sockets are released. */
static BOOL WINAPI convey_ctrl_handler(DWORD type)
{/*{{{*/
	switch (type) {
	case CTRL_C_EVENT:
	case CTRL_BREAK_EVENT:
	case CTRL_CLOSE_EVENT:
	case CTRL_LOGOFF_EVENT:
	case CTRL_SHUTDOWN_EVENT:
		convey_quit(0);
		return TRUE;
	default:
		return FALSE;
	}
}/*}}}*/

static void convey_do_command(char cmd)
{/*{{{*/
	switch (cmd) {
	case 'q':
		if (conf.verbose) {
			std::cout << "\r\nconvey: quit\r\n" << std::flush;
		}
		convey_quit(0);
		break;
	case 'b':
		if (INVALID_HANDLE_VALUE != pipe && FILE_TYPE_CHAR == GetFileType(pipe)) {
			SetCommBreak(pipe);
			std::this_thread::sleep_for(std::chrono::milliseconds(conf.break_ms));
			ClearCommBreak(pipe);
			if (conf.verbose) {
				std::cout << "\r\nconvey: break sent\r\n" << std::flush;
			}
		} else {
			std::cerr << "\r\nconvey: break is only supported on a serial port\r\n";
		}
		break;
	case 'r':
		if (INVALID_HANDLE_VALUE != pipe && FILE_TYPE_CHAR == GetFileType(pipe)) {
			convey_apply_line(pipe, true, convey_line_pulse);
			if (conf.verbose) {
				std::cout << "\r\nconvey: dtr reset pulse\r\n" << std::flush;
			}
		} else {
			std::cerr << "\r\nconvey: line control is only supported on a serial port\r\n";
		}
		break;
	case 'm':
		if (INVALID_HANDLE_VALUE != pipe && FILE_TYPE_CHAR == GetFileType(pipe)) {
			DWORD st = 0;
			if (GetCommModemStatus(pipe, &st)) {
				std::cout << "\r\nconvey: modem"
					<< " cts " << ((st & MS_CTS_ON) ? "on" : "off")
					<< ", dsr " << ((st & MS_DSR_ON) ? "on" : "off")
					<< ", dcd " << ((st & MS_RLSD_ON) ? "on" : "off")
					<< ", ri " << ((st & MS_RING_ON) ? "on" : "off")
					<< "\r\n" << std::flush;
			} else {
				std::cerr << "\r\nconvey: could not read the modem status\r\n";
			}
		} else {
			std::cerr << "\r\nconvey: modem status is only supported on a serial port\r\n";
		}
		break;
	case 'h':
		hex_on = !hex_on;
		std::cout << "\r\nconvey: hex " << (hex_on ? "on" : "off") << "\r\n" << std::flush;
		break;
	case 'e':
		echo_on = !echo_on;
		std::cout << "\r\nconvey: local echo " << (echo_on ? "on" : "off") << "\r\n" << std::flush;
		break;
	case 'l':
		if (INVALID_HANDLE_VALUE != log_handle || INVALID_HANDLE_VALUE != log_recv_handle || INVALID_HANDLE_VALUE != log_send_handle) {
			logging_on = !logging_on;
			std::cout << "\r\nconvey: logging " << (logging_on ? "on" : "off") << "\r\n" << std::flush;
		} else {
			// No log configured yet: start one under an auto-named file.
			auto pad = [](unsigned v, size_t w) {
				std::string s = std::to_string(v);
				return std::string(w > s.size() ? w - s.size() : 0, '0') + s;
			};
			SYSTEMTIME st;
			GetLocalTime(&st);
			std::string name = "convey-" + pad(st.wYear, 4) + pad(st.wMonth, 2) + pad(st.wDay, 2)
				+ "-" + pad(st.wHour, 2) + pad(st.wMinute, 2) + pad(st.wSecond, 2) + ".log";
			if (convey_open_log(name, log_handle)) {
				logging_on = true;
				std::cout << "\r\nconvey: logging to " << name << "\r\n" << std::flush;
			} else {
				std::cerr << "\r\nconvey: could not open " << name << "\r\n";
			}
		}
		break;
	case 'c':
		if (is_console && !conf.no_xterm) {
			std::cout << "\x1b[2J\x1b[3J\x1b[H" << std::flush;
		}
		break;
	case 'x':
		if (is_console) {
			std::string path;
			if (convey_prompt_line("send file: ", path)) {
				convey_send_file(path);
			}
		} else if (!conf.send_file.empty()) {
			convey_send_file(conf.send_file);
		} else {
			std::cerr << "\r\nconvey: no file to send\r\n";
		}
		break;
	case '.': {
		const char* tp = "pipe";
		if (convey_transport_is_tcp()) {
			tp = "tcp";
		} else if (convey_transport_is_unix()) {
			tp = "unix";
		} else if (INVALID_HANDLE_VALUE != pipe && FILE_TYPE_CHAR == GetFileType(pipe)) {
			tp = "serial";
		}
		std::cout << "\r\nconvey: " << tp << " " << conf.pipe_path << ", "
			<< (INVALID_HANDLE_VALUE == pipe ? "disconnected" : "connected")
			<< ", hex " << (hex_on ? "on" : "off")
			<< ", read-only " << (conf.read_only ? "on" : "off")
			<< ", rx " << bytes_rx.load() << ", tx " << bytes_tx.load()
			<< ", up " << convey_fmt_uptime(GetTickCount64() - conn_since.load()) << "\r\n" << std::flush;
		break;
	}
	case '?':
		std::cout << "\r\n";
		convey_key_help(std::cout, "\r\n");
		std::cout << std::flush;
		break;
	default: {
		unsigned char c = static_cast<unsigned char>(cmd);
		if (c >= 0x20 && c < 0x7f) {
			std::cerr << "\r\nconvey: no command for '" << static_cast<char>(c) << "', press ? for the list\r\n";
		} else {
			char hx[8];
			wsprintfA(hx, "0x%02x", c);
			std::cerr << "\r\nconvey: no command for " << hx << ", press ? for the list\r\n";
		}
		break;
	}
	}
}/*}}}*/

static convey_setup_status convey_startup(int argc, char **argv)
{/*{{{*/
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
	_setmode(_fileno(stderr), _O_BINARY);

	SetConsoleCtrlHandler(convey_ctrl_handler, TRUE);

	convey_setup_status rc = convey_conf_setup(argc, argv);
	if (convey_setup_ok != rc) {
		return rc;
	}

	if (!conf.bridge) {
		in = GetStdHandle(STD_INPUT_HANDLE);
		out = GetStdHandle(STD_OUTPUT_HANDLE);
		in_is_pipe = GetFileType(in) == FILE_TYPE_PIPE;
		out_is_pipe = GetFileType(out) == FILE_TYPE_PIPE;
		is_console = is_console_handle(in) && is_console_handle(out);
		if (is_console) {
			setup_console();

			// conf.escape is a control byte; OR 0x40 back to its letter.
			char k = static_cast<char>(conf.escape | 0x40);
			std::cerr << "convey: press Ctrl+" << k << " then ? for the list of commands\r\n";

			/* One persistent watcher owns the console input for the whole run,
			 * so the ctrl-a commands work even while waiting for a connection.
			 * Redirected input is handled per connection so piped data is not
			 * consumed before there is a target. */
			std::thread([]() {
				static bool esc_pending = false;
				while (true) {
					char buf[BUF_SIZE];
					DWORD bytes{0}, er{0};

					if (!convey_read_console(in, buf, sizeof buf, &bytes)) {
						return;
					}

					std::string cmds;
					bytes = convey_filter_escape(buf, bytes, esc_pending, cmds, conf.escape);
					for (size_t k = 0; k < cmds.size(); ++k) {
						convey_do_command(cmds[k]);
					}

					if (bytes && !conf.read_only && INVALID_HANDLE_VALUE != pipe) {
						convey_echo_local(buf, bytes);
						std::string s = convey_encode_eol(buf, bytes, conf.eol, conf.no_xterm);
						convey_log_sent(s.data(), static_cast<DWORD>(s.size()));
						DWORD w = static_cast<DWORD>(s.size());
						convey_write_pipe(pipe, s.data(), &w, e_pipe_w, er);
					} else if (0 == bytes) {
						std::this_thread::sleep_for(std::chrono::milliseconds(3));
					}
				}
			}).detach();
		}
	}

	return convey_setup_ok;
}/*}}}*/

int main(int argc, char** argv)
{/*{{{*/

	atexit(convey_final_cleanup);

	switch (convey_startup(argc, argv)) {
		case convey_setup_ok:
			break;
		case convey_setup_exit_err:
			return 1;
		case convey_setup_exit_ok:
			return 0;
	}

	if (have_exit && conf.exit_timeout > 0) {
		uint32_t ms = conf.exit_timeout * 1000;
		std::thread([ms]() {
			std::this_thread::sleep_for(std::chrono::milliseconds(ms));
			if (!exit_matched) {
				std::cerr << "\r\nconvey: --exit-on timed out\r\n";
				convey_quit(2);
			}
		}).detach();
	}

	if (!expect_steps.empty() && conf.expect_timeout > 0) {
		uint32_t ms = conf.expect_timeout * 1000;
		std::thread([ms]() {
			std::this_thread::sleep_for(std::chrono::milliseconds(ms));
			if (!expect_done) {
				std::cerr << "\r\nconvey: --expect sequence timed out\r\n";
				convey_quit(2);
			}
		}).detach();
	}

restart:
	switch (convey_connect(argc, argv)) {
		case convey_setup_ok:
			// pass
			break;
		case convey_setup_exit_err:
			return 1;
		case convey_setup_exit_ok:
			return 0;
	}

	if (!conf.bridge && !conf.send_file.empty() && !file_sent) {
		file_sent = true;
		std::string path = conf.send_file;
		std::thread([path]() { convey_send_file(path); }).detach();
	}

	if (!conf.bridge && !conf.read_only && !conf.send_init.empty()) {
		HANDLE ev = CreateEvent(nullptr, TRUE, FALSE, nullptr);
		DWORD n = static_cast<DWORD>(conf.send_init.size()), er = 0;
		convey_write_pipe(pipe, conf.send_init.data(), &n, ev, er);
		CloseHandle(ev);
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
				if (!rc || (convey_transport_is_socket() && 0 == bytes)) {
					if (!rc && !is_error && convey_report_io_error()) {
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
						if (!is_error && convey_report_io_error()) {
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
			is_reconnect = true;
			goto restart;
		}

		return 0;
	}

	std::thread t0([]() {
		if (is_console) {
			return;
		}
		static bool esc_pending = false;
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

			std::string cmds;
			bytes = convey_filter_escape(buf, bytes, esc_pending, cmds, conf.escape);
			for (size_t k = 0; k < cmds.size(); ++k) {
				convey_do_command(cmds[k]);
			}

			if (bytes && !conf.read_only) {
				convey_echo_local(buf, bytes);
				std::string s = convey_encode_eol(buf, bytes, conf.eol, conf.no_xterm);
				DWORD w = static_cast<DWORD>(s.size());
				convey_log_sent(s.data(), w);
				rc = convey_write_pipe(pipe, s.data(), &w, e_pipe_w, er);
				if (!rc) {
					if (!is_error && convey_report_io_error()) {
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
				if (!is_error && convey_report_io_error()) {
					convey_error(er);
				}
				convey_console_fail();
				return;
			}

			if (convey_transport_is_socket() && 0 == bytes) {
				convey_console_fail();
				return;
			}

			if (bytes) {
				convey_log_recv(buf, bytes);
				convey_exit_feed(buf, bytes);
				convey_expect_feed(buf, bytes);
				const char* wbuf = buf;
				DWORD wbytes = bytes;
				std::string hexbuf, tsbuf;
				if (hex_on) {
					static size_t hex_offset = 0;
					convey_hexdump(wbuf, wbytes, hex_offset, hexbuf);
					wbuf = hexbuf.data();
					wbytes = (DWORD)hexbuf.size();
				}
				if (conf.timestamps) {
					static bool ts_line_start = true;
					convey_stamp_lines(wbuf, wbytes, ts_line_start, tsbuf);
					wbuf = tsbuf.data();
					wbytes = (DWORD)tsbuf.size();
				}
				DWORD wb = wbytes;
				if (out_is_pipe) {
					rc = convey_write_pipe(out, wbuf, &wb, e_out, er);
				} else {
					rc = WriteFile(out, wbuf, wbytes, &wb, nullptr);
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

	t0.join();
	t1.join();

	convey_shutdown();

	if (restart_on_exit) {
		is_error = false;
		shutting_down = false;
		is_reconnect = true;
		goto restart;
	}

	return 0;
}/*}}}*/
#endif

