// Unit tests for the pure logic in main.cxx.
// Defines CONVEY_UNIT_TEST so main.cxx omits its own main(), then includes it
// and exercises its static functions directly.
#define CONVEY_UNIT_TEST
#include "../main.cxx"

static int g_fail = 0;
#define EXPECT(c) do { \
	if (!(c)) { std::cerr << "FAIL: " #c << std::endl; ++g_fail; } \
	else { std::cout << "PASS: " #c << std::endl; } \
} while (0)

// Drive convey_conf_setup with a fresh global conf and argv built from args.
static convey_setup_status run_setup(std::vector<const char*> args)
{
	conf = convey_conf{};
	restart_on_exit = false;
	std::vector<char*> argv;
	for (const char* a : args) {
		argv.push_back(const_cast<char*>(a));
	}
	return convey_conf_setup(static_cast<int>(argv.size()), argv.data());
}

int main()
{
	{
		convey_transport_spec s = convey_parse_transport("tcp:127.0.0.1:4445");
		EXPECT(s.ok);
		EXPECT(s.kind == convey_tp_tcp_client);
		EXPECT(s.host == "127.0.0.1");
		EXPECT(s.port == "4445");
	}
	{
		convey_transport_spec s = convey_parse_transport("tcp:server.local:22");
		EXPECT(s.ok);
		EXPECT(s.kind == convey_tp_tcp_client);
		EXPECT(s.host == "server.local");
		EXPECT(s.port == "22");
	}
	{
		convey_transport_spec s = convey_parse_transport("tcp-listen:5000");
		EXPECT(s.ok);
		EXPECT(s.kind == convey_tp_tcp_server);
		EXPECT(s.port == "5000");
	}
	{
		convey_transport_spec s = convey_parse_transport("\\\\.\\pipe\\kd0");
		EXPECT(s.ok);
		EXPECT(s.kind == convey_tp_pipe);
	}
	{
		convey_transport_spec s = convey_parse_transport("COM3");
		EXPECT(s.ok);
		EXPECT(s.kind == convey_tp_pipe);
	}
	{
		convey_transport_spec s = convey_parse_transport("tcp:");
		EXPECT(!s.ok);
		EXPECT(s.kind == convey_tp_tcp_client);
	}
	{
		convey_transport_spec s = convey_parse_transport("tcp:host");
		EXPECT(!s.ok);
	}
	{
		convey_transport_spec s = convey_parse_transport("tcp:host:");
		EXPECT(!s.ok);
	}
	{
		convey_transport_spec s = convey_parse_transport("tcp-listen:");
		EXPECT(!s.ok);
		EXPECT(s.kind == convey_tp_tcp_server);
	}
	{
		convey_transport_spec s = convey_parse_transport("unix:/tmp/convey.sock");
		EXPECT(s.ok);
		EXPECT(s.kind == convey_tp_unix_client);
		EXPECT(s.host == "/tmp/convey.sock");
	}
	{
		convey_transport_spec s = convey_parse_transport("unix-listen:C:\\tmp\\convey.sock");
		EXPECT(s.ok);
		EXPECT(s.kind == convey_tp_unix_server);
		EXPECT(s.host == "C:\\tmp\\convey.sock");
	}
	{
		convey_transport_spec s = convey_parse_transport("unix:");
		EXPECT(!s.ok);
		EXPECT(s.kind == convey_tp_unix_client);
	}
	{
		convey_transport_spec s = convey_parse_transport("unix-listen:");
		EXPECT(!s.ok);
		EXPECT(s.kind == convey_tp_unix_server);
	}
	{
		convey_transport_spec s = convey_parse_transport("pipe-listen:\\\\.\\pipe\\relay");
		EXPECT(s.ok);
		EXPECT(s.kind == convey_tp_pipe_server);
		EXPECT(s.host == "\\\\.\\pipe\\relay");
	}
	{
		convey_transport_spec s = convey_parse_transport("pipe-listen:");
		EXPECT(!s.ok);
		EXPECT(s.kind == convey_tp_pipe_server);
	}

	EXPECT(convey_parity_from_string("even") == EVENPARITY);
	EXPECT(convey_parity_from_string("NO") == NOPARITY);
	EXPECT(convey_parity_from_string("Space") == SPACEPARITY);
	EXPECT(convey_parity_from_string("bogus") == (uint8_t)-1);

	EXPECT(convey_stop_bits_from_string("1") == ONESTOPBIT);
	EXPECT(convey_stop_bits_from_string("1.5") == ONE5STOPBITS);
	EXPECT(convey_stop_bits_from_string("2") == TWOSTOPBITS);
	EXPECT(convey_stop_bits_from_string("3") == (uint8_t)-1);

	EXPECT(convey_flow_control_from_string("none") == convey_flow_control_none);
	EXPECT(convey_flow_control_from_string("XON/XOFF") == convey_flow_control_xonxoff);
	EXPECT(convey_flow_control_from_string("rts/cts") == convey_flow_control_rtscts);
	EXPECT(convey_flow_control_from_string("dsr/dtr") == convey_flow_control_dsrdtr);
	EXPECT(convey_flow_control_from_string("bad") == (convey_flow_control)-1);

	EXPECT(convey_baud_is_valid(CBR_9600));
	EXPECT(convey_baud_is_valid(CBR_115200));
	EXPECT(convey_baud_is_valid(250000));
	EXPECT(convey_baud_is_valid(921600));
	EXPECT(!convey_baud_is_valid(0));

	EXPECT(convey_trim_crlf("ab\r\n", 4) == 3);
	EXPECT(convey_trim_crlf("ab\n", 3) == 3);
	EXPECT(convey_trim_crlf("\r\n", 2) == 1);
	EXPECT(convey_trim_crlf("", 0) == 0);
	EXPECT(convey_trim_crlf("x", 1) == 1);

	// Uptime formatting picks the compact unit set for the magnitude.
	EXPECT(convey_fmt_uptime(0) == "0s");
	EXPECT(convey_fmt_uptime(5000) == "5s");
	EXPECT(convey_fmt_uptime(64000) == "1m 04s");
	EXPECT(convey_fmt_uptime(3723000) == "1h 02m 03s");

	{
		// Socket teardown errors stay quiet unless verbose; pipe and serial report.
		conf = convey_conf{};
		conf.transport = convey_tp_unix_server;
		EXPECT(!convey_report_io_error());
		conf.transport = convey_tp_tcp_client;
		EXPECT(!convey_report_io_error());
		conf.verbose = true;
		EXPECT(convey_report_io_error());
		conf.verbose = false;
		conf.transport = convey_tp_pipe;
		EXPECT(convey_report_io_error());
	}

	{
		char rec[16];
		DWORD n = convey_log_session_record(rec, "hi", 2, true);
		EXPECT(n == 4);
		EXPECT(rec[0] == '>');
		EXPECT(rec[1] == ' ');
		EXPECT(rec[2] == 'h');
		EXPECT(rec[3] == 'i');
	}
	{
		char rec[16];
		DWORD n = convey_log_session_record(rec, "x", 1, false);
		EXPECT(n == 3);
		EXPECT(rec[0] == '<');
		EXPECT(rec[2] == 'x');
	}
	{
		// timestamps prefix each line and preserve the payload
		bool ls = true;
		std::string out;
		convey_stamp_lines("ab\ncd", 5, ls, out);
		size_t brackets = 0;
		for (size_t i = 0; i < out.size(); ++i) { if (out[i] == '[') ++brackets; }
		EXPECT(brackets == 2);
		EXPECT(!ls);
		EXPECT(out.find("ab\n") != std::string::npos);
		EXPECT(out.find("cd") != std::string::npos);
	}
	{
		// a trailing newline leaves the next write at a line start
		bool ls = true;
		std::string out;
		convey_stamp_lines("x\n", 2, ls, out);
		EXPECT(ls);
	}
	{
		// hex dump shows the offset, the bytes and an ascii gutter
		size_t off = 0;
		std::string out;
		convey_hexdump("KD", 2, off, out);
		EXPECT(off == 2);
		EXPECT(out.find("00000000") != std::string::npos);
		EXPECT(out.find("4b 44") != std::string::npos);
		EXPECT(out.find("|KD|") != std::string::npos);
	}
	{
		// a non printable byte shows as a dot in the gutter
		size_t off = 0;
		std::string out;
		convey_hexdump("\x01" "A", 2, off, out);
		EXPECT(out.find("01 41") != std::string::npos);
		EXPECT(out.find("|.A|") != std::string::npos);
	}
	{
		// the offset advances across calls
		size_t off = 0;
		std::string out;
		convey_hexdump("ab", 2, off, out);
		convey_hexdump("cd", 2, off, out);
		EXPECT(off == 4);
		EXPECT(out.find("00000002") != std::string::npos);
	}
	{
		// ctrl-a q is removed from the stream and yields the q command
		bool esc = false;
		std::string cmds;
		char b[] = { 'h', 'i', 0x01, 'q', 'x' };
		DWORD n = convey_filter_escape(b, 5, esc, cmds, 0x01);
		EXPECT(n == 3);
		EXPECT(b[0] == 'h' && b[1] == 'i' && b[2] == 'x');
		EXPECT(cmds == "q");
		EXPECT(!esc);
	}
	{
		// an escape at the end of a read leaves the command pending for the next read
		bool esc = false;
		std::string cmds;
		char b1[] = { 'a', 0x01 };
		DWORD n1 = convey_filter_escape(b1, 2, esc, cmds, 0x01);
		EXPECT(n1 == 1);
		EXPECT(esc);
		EXPECT(cmds.empty());
		char b2[] = { 'q' };
		DWORD n2 = convey_filter_escape(b2, 1, esc, cmds, 0x01);
		EXPECT(n2 == 0);
		EXPECT(!esc);
		EXPECT(cmds == "q");
	}
	{
		// a custom escape byte is honored and the default 0x01 passes through
		bool esc = false;
		std::string cmds;
		char b[] = { 'a', 0x1d, 'q', 0x01 };
		DWORD n = convey_filter_escape(b, 4, esc, cmds, 0x1d);
		EXPECT(n == 2);
		EXPECT(b[0] == 'a' && b[1] == 0x01);
		EXPECT(cmds == "q");
		// escape key specs parse to control bytes, invalid ones rejected
		EXPECT(convey_parse_escape("^A") == 0x01);
		EXPECT(convey_parse_escape("ctrl-]") == 0x1d);
		EXPECT(convey_parse_escape("C-a") == 0x01);
		EXPECT(convey_parse_escape("ctrl+b") == 0x02);
		EXPECT(convey_parse_escape("c+b") == 0x02);
		EXPECT(convey_parse_escape("nope") == -1);
		EXPECT(run_setup({"convey", "--escape", "^]", "COM1"}) == convey_setup_ok);
		EXPECT(conf.escape == 0x1d);
		EXPECT(run_setup({"convey", "COM1"}) == convey_setup_ok);
		EXPECT(conf.escape == 0x01);
		EXPECT(run_setup({"convey", "--escape", "bogus", "COM1"}) == convey_setup_exit_err);
	}

	// --- convey_conf_setup: parser functional-equivalence coverage ---
	{
		// Endpoint given as the first positional argument.
		EXPECT(run_setup({"convey", "tcp:127.0.0.1:4445"}) == convey_setup_ok);
		EXPECT(conf.pipe_path == "tcp:127.0.0.1:4445");
		EXPECT(conf.transport == convey_tp_tcp_client);
		EXPECT(conf.tcp_host == "127.0.0.1");
		EXPECT(conf.tcp_port == "4445");
	}
	{
		// A unix endpoint stores the socket path.
		EXPECT(run_setup({"convey", "unix:C:\\tmp\\convey.sock"}) == convey_setup_ok);
		EXPECT(conf.transport == convey_tp_unix_client);
		EXPECT(conf.tcp_host == "C:\\tmp\\convey.sock");
		EXPECT(run_setup({"convey", "unix-listen:C:\\tmp\\convey.sock"}) == convey_setup_ok);
		EXPECT(conf.transport == convey_tp_unix_server);
		EXPECT(conf.tcp_host == "C:\\tmp\\convey.sock");
	}
	{
		// --dev is an alias for the positional endpoint.
		EXPECT(run_setup({"convey", "--dev", "\\\\.\\pipe\\kd0"}) == convey_setup_ok);
		EXPECT(conf.pipe_path == "\\\\.\\pipe\\kd0");
		EXPECT(conf.transport == convey_tp_pipe);
	}
	{
		// tcp-listen endpoint.
		EXPECT(run_setup({"convey", "tcp-listen:5000"}) == convey_setup_ok);
		EXPECT(conf.transport == convey_tp_tcp_server);
		EXPECT(conf.tcp_port == "5000");
	}
	{
		// A missing endpoint is an error.
		EXPECT(run_setup({"convey"}) == convey_setup_exit_err);
	}
	{
		// An unknown option is an error.
		EXPECT(run_setup({"convey", "--nope", "COM1"}) == convey_setup_exit_err);
	}
	{
		// Defaults match the documented values.
		EXPECT(run_setup({"convey", "COM1"}) == convey_setup_ok);
		EXPECT(conf.baud == CBR_115200);
		EXPECT(conf.parity == NOPARITY);
		EXPECT(conf.stop_bits == ONESTOPBIT);
		EXPECT(conf.byte_size == 8);
		EXPECT(conf.flow_control == convey_flow_control_none);
		EXPECT(conf.break_ms == 300);
		EXPECT(conf.reset_ms == 120);
		EXPECT(!conf.wait);
		EXPECT(!restart_on_exit);
	}
	{
		// --wait enables the indefinite startup wait.
		EXPECT(run_setup({"convey", "--wait", "COM1"}) == convey_setup_ok);
		EXPECT(conf.wait);
	}
	{
		// Serial parameters are parsed and mapped.
		EXPECT(run_setup({"convey", "--baud", "9600", "--parity", "even",
			"--stop-bits", "2", "--byte-size", "7", "--flow-control", "rts/cts", "COM1"}) == convey_setup_ok);
		EXPECT(conf.baud == CBR_9600);
		EXPECT(conf.parity == EVENPARITY);
		EXPECT(conf.stop_bits == TWOSTOPBITS);
		EXPECT(conf.byte_size == 7);
		EXPECT(conf.flow_control == convey_flow_control_rtscts);
	}
	{
		// A custom baud rate within range is accepted; the driver is the final say.
		EXPECT(run_setup({"convey", "--baud", "250000", "COM1"}) == convey_setup_ok);
		EXPECT(conf.baud == 250000);
	}
	{
		// The BREAK hold time is configurable.
		EXPECT(run_setup({"convey", "--break-ms", "500", "COM1"}) == convey_setup_ok);
		EXPECT(conf.break_ms == 500);
	}
	{
		// The DTR/RTS pulse width is configurable.
		EXPECT(run_setup({"convey", "--reset-ms", "50", "COM1"}) == convey_setup_ok);
		EXPECT(conf.reset_ms == 50);
	}
	{
		// Invalid serial values are rejected.
		EXPECT(run_setup({"convey", "--baud", "0", "COM1"}) == convey_setup_exit_err);
		EXPECT(run_setup({"convey", "--parity", "bogus", "COM1"}) == convey_setup_exit_err);
		EXPECT(run_setup({"convey", "--stop-bits", "3", "COM1"}) == convey_setup_exit_err);
		EXPECT(run_setup({"convey", "--flow-control", "bad", "COM1"}) == convey_setup_exit_err);
	}
	{
		// --reconnect arms the restart loop.
		EXPECT(run_setup({"convey", "--reconnect", "tcp:127.0.0.1:9"}) == convey_setup_ok);
		EXPECT(restart_on_exit);
	}
	{
		// --bridge requires --pipe-server, then arms the restart loop.
		EXPECT(run_setup({"convey", "--bridge", "tcp:127.0.0.1:9"}) == convey_setup_exit_err);
		EXPECT(run_setup({"convey", "--bridge", "--pipe-server", "\\\\.\\pipe\\b", "tcp:127.0.0.1:9"}) == convey_setup_ok);
		EXPECT(conf.bridge);
		EXPECT(conf.bridge_pipe_name == "\\\\.\\pipe\\b");
		EXPECT(restart_on_exit);
	}
	{
		// --relay takes two endpoint specs and sets relay mode.
		EXPECT(run_setup({"convey", "--relay", "tcp-listen:5000", "tcp:127.0.0.1:9"}) == convey_setup_ok);
		EXPECT(conf.relay);
		EXPECT(conf.relay_a == "tcp-listen:5000");
		EXPECT(conf.relay_b == "tcp:127.0.0.1:9");
		// A bad relay endpoint is rejected.
		EXPECT(run_setup({"convey", "--relay", "tcp:nohost", "tcp:127.0.0.1:9"}) == convey_setup_exit_err);
	}
	{
		// Log options land in the right fields.
		EXPECT(run_setup({"convey", "--log", "a.log", "--log-recv", "b.log",
			"--log-send", "c.log", "--log-append", "COM1"}) == convey_setup_ok);
		EXPECT(conf.log_path == "a.log");
		EXPECT(conf.log_recv_path == "b.log");
		EXPECT(conf.log_send_path == "c.log");
		EXPECT(conf.log_append);
	}
	{
		// Two log options may not share a file.
		EXPECT(run_setup({"convey", "--log", "x.log", "--log-recv", "x.log", "COM1"}) == convey_setup_exit_err);
	}
	{
		// --no-xterm and --verbose flags.
		EXPECT(run_setup({"convey", "--no-xterm", "--verbose", "COM1"}) == convey_setup_ok);
		EXPECT(conf.no_xterm);
		EXPECT(conf.verbose);
	}
	{
		// read only sets monitor mode off by default.
		EXPECT(run_setup({"convey", "COM1"}) == convey_setup_ok);
		EXPECT(!conf.read_only);
		EXPECT(run_setup({"convey", "--read-only", "COM1"}) == convey_setup_ok);
		EXPECT(conf.read_only);
	}
	{
		// eol auto: raw with xterm, drop the trailing LF (keep CR) with --no-xterm.
		EXPECT(convey_encode_eol("a\r\nb", 4, convey_eol_auto, false) == std::string("a\r\nb", 4));
		EXPECT(convey_encode_eol("a\r\n", 3, convey_eol_auto, true) == std::string("a\r", 2));
		EXPECT(convey_encode_eol("a\r\nb\r\n", 6, convey_eol_auto, true) == std::string("a\r\nb\r", 5));
		// raw sends bytes unchanged even with --no-xterm.
		EXPECT(convey_encode_eol("a\r\n", 3, convey_eol_raw, true) == std::string("a\r\n", 3));
		// lf converts every CRLF to LF and leaves a lone CR alone.
		EXPECT(convey_encode_eol("a\r\nb\r\n", 6, convey_eol_lf, false) == std::string("a\nb\n", 4));
		EXPECT(convey_encode_eol("a\rb", 3, convey_eol_lf, false) == std::string("a\rb", 3));
		// crlf adds CR to a lone LF and never doubles an existing CRLF.
		EXPECT(convey_encode_eol("a\nb\r\n", 5, convey_eol_crlf, false) == std::string("a\r\nb\r\n", 6));
		EXPECT(convey_encode_eol("a\r\n", 3, convey_eol_crlf, false) == std::string("a\r\n", 3));
	}
	{
		// The newline flags are mutually exclusive.
		EXPECT(run_setup({"convey", "--lf", "COM1"}) == convey_setup_ok);
		EXPECT(conf.eol == convey_eol_lf);
		EXPECT(run_setup({"convey", "--crlf", "COM1"}) == convey_setup_ok);
		EXPECT(conf.eol == convey_eol_crlf);
		EXPECT(run_setup({"convey", "--raw", "COM1"}) == convey_setup_ok);
		EXPECT(conf.eol == convey_eol_raw);
		EXPECT(run_setup({"convey", "--lf", "--crlf", "COM1"}) == convey_setup_exit_err);
	}
	{
		// Backoff doubles the delay up to the cap.
		EXPECT(convey_backoff_next(300, 5000) == 600);
		EXPECT(convey_backoff_next(600, 5000) == 1200);
		EXPECT(convey_backoff_next(3000, 5000) == 5000);
		EXPECT(convey_backoff_next(5000, 5000) == 5000);
		// --reconnect and --max-retries land in conf.
		EXPECT(run_setup({"convey", "--reconnect", "tcp:127.0.0.1:9"}) == convey_setup_ok);
		EXPECT(conf.reconnect);
		EXPECT(conf.max_retries == 0);
		EXPECT(run_setup({"convey", "--reconnect", "--max-retries", "5", "tcp:127.0.0.1:9"}) == convey_setup_ok);
		EXPECT(conf.max_retries == 5);
	}
	{
		// DTR/RTS states parse, and invalid ones are rejected.
		EXPECT(convey_line_from_string("on") == convey_line_on);
		EXPECT(convey_line_from_string("off") == convey_line_off);
		EXPECT(convey_line_from_string("pulse") == convey_line_pulse);
		EXPECT(convey_line_from_string("bogus") == (convey_line)-1);
		EXPECT(run_setup({"convey", "--dtr", "on", "--rts", "pulse", "COM1"}) == convey_setup_ok);
		EXPECT(conf.dtr == convey_line_on);
		EXPECT(conf.rts == convey_line_pulse);
		EXPECT(run_setup({"convey", "COM1"}) == convey_setup_ok);
		EXPECT(conf.dtr == convey_line_keep);
		EXPECT(conf.rts == convey_line_keep);
		EXPECT(run_setup({"convey", "--dtr", "bogus", "COM1"}) == convey_setup_exit_err);
	}
	{
		// convey_unescape decodes backslash escapes, rejecting a bad \x.
		std::string out;
		out.clear(); EXPECT(convey_unescape("abc", out) && out == "abc");
		std::string crlf; crlf.push_back('a'); crlf.push_back('\r'); crlf.push_back('\n'); crlf.push_back('b');
		out.clear(); EXPECT(convey_unescape("a\\r\\nb", out) && out == crlf);
		out.clear(); EXPECT(convey_unescape("\\x41", out) && out == "A");
		std::string esc; esc.push_back('\x1b');
		out.clear(); EXPECT(convey_unescape("\\x1b", out) && out == esc);
		out.clear(); EXPECT(convey_unescape("\\q", out) && out == "q");
		out.clear(); EXPECT(!convey_unescape("\\xZZ", out));
		// --send-init decodes into conf; a bad escape is rejected.
		EXPECT(run_setup({"convey", "--send-init", "hi\\r\\n", "COM1"}) == convey_setup_ok);
		std::string want; want += "hi"; want.push_back('\r'); want.push_back('\n');
		EXPECT(conf.send_init == want);
		EXPECT(run_setup({"convey", "--send-init", "\\xZZ", "COM1"}) == convey_setup_exit_err);
	}
	{
		// --exit-on requires a valid regex and captures the timeout.
		EXPECT(run_setup({"convey", "--exit-on", "READY", "COM1"}) == convey_setup_ok);
		EXPECT(run_setup({"convey", "--exit-on", "R.DY", "--exit-timeout", "5", "COM1"}) == convey_setup_ok);
		EXPECT(conf.exit_timeout == 5);
		EXPECT(run_setup({"convey", "--exit-on", "[", "COM1"}) == convey_setup_exit_err);
	}
	{
		// --expect pairs with --reply; counts must match and regexes be valid.
		EXPECT(run_setup({"convey", "--expect", "login:", "--reply", "root\\r", "COM1"}) == convey_setup_ok);
		EXPECT(expect_steps.size() == 1);
		EXPECT(run_setup({"convey", "--expect", "login:", "COM1"}) == convey_setup_exit_err);
		EXPECT(run_setup({"convey", "--expect", "[", "--reply", "x", "COM1"}) == convey_setup_exit_err);
	}
	{
		// convey_tcp_tune turns on NODELAY and keepalive on a data socket.
		WSADATA wsa;
		EXPECT(0 == WSAStartup(MAKEWORD(2, 2), &wsa));
		SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		EXPECT(INVALID_SOCKET != s);
		convey_tcp_tune(s);

		int val = 0, len = sizeof val;
		EXPECT(0 == getsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&val), &len));
		EXPECT(0 != val);
		val = 0; len = sizeof val;
		EXPECT(0 == getsockopt(s, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<char*>(&val), &len));
		EXPECT(0 != val);

		closesocket(s);
		WSACleanup();
	}

	if (g_fail) {
		std::cerr << g_fail << " unit test(s) failed." << std::endl;
		return 1;
	}
	std::cout << "All unit tests passed." << std::endl;
	return 0;
}
