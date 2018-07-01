// HTTP/S Broker (reverse proxy). Allows for incoming HTTP and HTTPS requests to be listened for (served from) on the same TCP port. Incoming requests are forwarded (reverse proxied) to other web servers listening on other ports on the local machine, based on whether the incoming request is HTTP or HTTPS

// By Theo Fitchat (Cluedapp)
// 2014-05-13 - 2014-05-14

// To compile: g++ http_broker.cpp --std=c++11 -O3 -lwsock32 -o http_broker.exe
// Run with: http_broker.exe -l 80 -p 8080 -s 443 --verbose

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>

#include <getopt.h>

#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

using namespace std;

#ifdef DEBUG
mutex outLock;
ofstream nul("nul"); // change to /dev/null on linux
string getErrorMessage(int);
#define LOG(x) outLock.lock(); cout << x << endl; outLock.unlock();
#define LOGERR(x, err) outLock.lock(); cout << x << getErrorMessage(err) << endl; outLock.unlock();
#else
#define LOG(x)
#define LOGERR(x, err)
#endif

int listenPort = 0, httpPort = 0, httpsPort = 0, sshPort = 0;

string getErrorMessage(int wsaErrorCode) {
	string msg;
	LPTSTR errorMsg = 0;

	if (FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM, NULL, wsaErrorCode, 0, (LPTSTR)&errorMsg, 0, NULL) != 0) {
		msg = errorMsg;
		LocalFree(errorMsg);
	}

	return msg;
}

void broker(SOCKET browser)
{
	// Client: Get Host
	LOG("Client: Create Host");

	// Now we can get to the real fun. On the client side, we are going to need to find our host based on their name. For example we may be trying to connect to the server async5-5.remote.ualberta.ca. We do this using the function gethostbyname.

	hostent *host;
	unsigned long addr;
	string hostname = "localhost"; // FIXME/TODO: make hostname non-hardcoded in future
	if (isalpha(hostname[0])) { // hostname is an ASCII name
		host = gethostbyname(hostname.c_str());
		addr = ((in_addr *) (host->h_addr))->s_addr;
	} else { // hostname is possibly in the form abc.def.ghi.jkl
		addr = inet_addr(hostname.c_str());
		if (addr == INADDR_NONE || addr == INADDR_ANY) {
			LOGERR("Invalid server address (" << hostname << "): ", WSAGetLastError());
			goto end;
		}
		/*
		} else {
			host = gethostbyaddr((char *) &addr, 4, AF_INET);
		}
		*/
	}

	// Client: Create Socket
	LOG("Client: Create Socket");

	SOCKET server;
	server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (server == INVALID_SOCKET) {
		LOGERR("Failed to open server socket: ", WSAGetLastError());
		goto end;
	}

	// Broker/proxy to correct reverse server port
	LOG("Initial read from client");

	int port;
	char serverBuf[65535], clientBuf[65535];
	int serverLen, clientLen;
	serverLen = 0;
	clientLen = recv(browser, clientBuf, sizeof clientBuf, 0);
	if (httpsPort && clientBuf[0] == 0x16) { // the HTTPS magic identifier!
		port = httpsPort;
	} else if (sshPort && clientLen >= 3 && clientBuf[0] == 'S' && clientBuf[1] == 'S' && clientBuf[2] == 'H') {
		port = sshPort;
	} else if (httpPort) {
		port = httpPort;
	} else {
		LOG("Unable to reverse proxy request due to invalid incoming data, or no reverse port available" << port);
		goto end;
	}

	LOG("Brokered to port " << port);

	// Client: Connecting to Server
	LOG("Connecting to Server");
	
	struct sockaddr_in sin;
	memset(&sin, 0, sizeof sin);

	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = addr;
	sin.sin_port = htons(port);

	if (connect(server, (struct sockaddr*) &sin, sizeof sin) != SOCKET_ERROR)
	{
		bool error = false;
		mutex serverLock, clientLock;

		// Set up the struct timeval for the timeout.
		struct timeval tv;
		tv.tv_sec = 10;
		tv.tv_usec = 0;

		thread t[] {
			thread([&] {
				while (true) {
					if (clientLen > 0) {

						LOG("Checking if server write possible");

						// Set up the file descriptor set
						fd_set fds;
						FD_ZERO(&fds);
						FD_SET(server, &fds);

						// Wait until timeout or acknowledgement that data can be sent successfully
						if (select(server + 1, NULL, &fds, NULL, &tv) != 1) {
							error = true;
							LOGERR("SELECT: Server write impossible: ", WSAGetLastError());
							break;
						}

						LOG("Writing to server");

						lock_guard<mutex> m(clientLock);

						if (send(server, clientBuf, clientLen, 0) <= 0) {
							error = true;
							LOGERR("SEND: Server connection ended: ", WSAGetLastError());
							break;
						}

						clientLen = 0;
					} else if (error) {
						break;
					}
				}
			}),
			thread([&] {
				while (true) {
					if (serverLen == 0) {

						LOG("Checking if server read possible");

						// Set up the file descriptor set
						fd_set fds;
						FD_ZERO(&fds);
						FD_SET(server, &fds);

						// Wait until timeout or acknowledgement that data is available to be read
						if (select(server + 1, &fds, NULL, NULL, &tv) != 1) {
							error = true;
							LOGERR("SELECT: Server read impossible: ", WSAGetLastError());
							break;
						}

						LOG("Reading from server");

						lock_guard<mutex> m(serverLock);

						if ((serverLen = recv(server, serverBuf, sizeof serverBuf, 0)) <= 0) {
							error = true;
							LOGERR("RECV: Server connection ended: ", WSAGetLastError());
							break;
						}
					} else if (error) {
						break;
					}
				}
			}),
			thread([&] {
				while (true) {
					if (serverLen > 0) {

						LOG("Checking if client write possible");

						// Set up the file descriptor set
						fd_set fds;
						FD_ZERO(&fds);
						FD_SET(browser, &fds);

						// Wait until timeout or acknowledgement that data can be sent successfully
						if (select(browser + 1, NULL, &fds, NULL, &tv) != 1) {
							//error = true;
							LOGERR("SELECT: Client write impossible: ", WSAGetLastError());
							break;
						}

						LOG("Writing to client");

						lock_guard<mutex> m(serverLock);

						if (send(browser, serverBuf, serverLen, 0) <= 0) {
							error = true;
							LOGERR("SEND: Client connection ended: ", WSAGetLastError());
							break;
						}
						serverLen = 0;
					} else if (error) {
						break;
					}
				}
			}),
			thread([&] {
				while (true) {
					if (clientLen == 0) {

						LOG("Checking if client read possible");

						// Set up the file descriptor set
						fd_set fds;
						FD_ZERO(&fds);
						FD_SET(browser, &fds);

						// Wait until timeout or acknowledgement that data is available to be read
						if (select(browser + 1, &fds, NULL, NULL, &tv) != 1) {
							//error = true;
							LOGERR("SELECT: Client read impossible: ", WSAGetLastError());
							break;
						}

						LOG("Reading from client");

						lock_guard<mutex> m(clientLock);

						if ((clientLen = recv(browser, clientBuf, sizeof clientBuf, 0)) <= 0) {
							error = true;
							LOGERR("RECV: Client connection ended: ", WSAGetLastError());
							break;
						}
					} else if (error) {
						break;
					}
				}
			})
		};
		
		for (int i = 0; i < ((sizeof t) / (sizeof t[0])); ++i) t[i].join();
	}
	else {
		/* could not connect to server */
		LOGERR("Could not connect to server: ", WSAGetLastError());
	}

	end:
	LOG("Closing server and client sockets");
    closesocket(browser);
	if (server != INVALID_SOCKET) {
		closesocket(server);
	}
}

void help() {
	cout <<
		"-h, --help         Show help message"
		"\n-l, --listen     <Port to listen on for incoming requests>"
		"\n-p, --http       <Port to forward HTTP requests to>"
		"\n-s, --https      <Port to forward HTTPS requests to>"
		"\n-t, --ssh        <Port to forward SSH requests to>"
		#ifdef DEBUG
		"\n-v, --verbose    Enable verbose mode"
		#endif
		;
	exit(0);
}

void handleCommandLine (int argc, char** argv) {
	static struct option long_options[] = {
		{"help", optional_argument, 0, 'h'},
		{"port", required_argument, 0, 'l'},
		{"http", required_argument, 0, 'p'},
		{"https", required_argument, 0, 's'},
		{"ssh", required_argument, 0, 't'},
		{"verbose", optional_argument, 0, 'v'},
		{0, 0, 0, 0}
	};
	int option_index = 0, option_count = 0;

	#ifdef DEBUG
	bool verbose = false;
	#endif

	while (1) {
		int c = getopt_long (argc, argv, "hl:p:s:t:v", long_options, &option_index);

		// Detect the end of the options
		if (c == -1)
			break;

		switch (c)
		{
			case 'h':
				help();
				break;

			case 'l':
				listenPort = atoi(optarg);
				++option_count;
				break;

			case 'p':
				httpPort = atoi(optarg);
				++option_count;
				break;

			case 's':
				httpsPort = atoi(optarg);
				++option_count;
				break;

			case 't':
				sshPort = atoi(optarg);
				++option_count;
				break;

			#ifdef DEBUG
			case 'v':
				verbose = true;
				break;
			#endif

			default:
				continue;
		}
	}

	if (listenPort == 0) {
		help();
	}

	#ifdef DEBUG
	if (!verbose) {
		cout.rdbuf(nul.rdbuf());
	}
	#endif
}

int main(int argc, char **argv) {
	handleCommandLine(argc, argv);

	// WinSock: Initializing

	LOG("WinSock: Initializing");

	// The first step is to call WSAStartup to startup the interface to WinSock.

	WSADATA wsaData;
	int version;
	int error;

	version = MAKEWORD(2, 2);
	error = WSAStartup(version, &wsaData);
	atexit([] { WSACleanup(); });
	
	/* check for error */
	if (error != 0)
	{
		/* error occurred */
		LOGERR("Initialization error: ", WSAGetLastError());
		return 1;
	}

	/* check for correct version */
	if (LOBYTE(wsaData.wVersion) != 2 ||
		HIBYTE(wsaData.wVersion) != 2)
	{
		/* incorrect WinSock version */
		/* WinSock has been initialized */
		LOGERR("Incorrect WinSock version: ", WSAGetLastError());
		return 1;
	}

	// Server: Create Socket

	LOG("Creating proxy server socket");

	SOCKET proxy;
	proxy = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

	if (proxy == INVALID_SOCKET) {
		LOGERR("Failed to open proxy server socket: ", WSAGetLastError());
		return 1;
	}

	// Server: Starting Server

	struct sockaddr_in sin;
	memset(&sin, 0, sizeof sin);

	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_ANY);
	sin.sin_port = htons(listenPort);

	LOG("Binding proxy server socket");

	if (bind(proxy, (struct sockaddr*) &sin, sizeof sin) == SOCKET_ERROR)
	{
		/* could not start server */
		LOGERR("Failed to bind proxy server socket: ", WSAGetLastError());
		return 1;
	}

	// Server: Listen for Client

	LOG("Listening for clients");

	while (listen(proxy, SOMAXCONN) != SOCKET_ERROR) {

		// Server: Accepting Connection

		LOG("Waiting to accept client connection");

		SOCKET browser;
		int length;

		length = sizeof sin;
		browser = accept(proxy, (struct sockaddr*) &sin, &length);

		LOG("Starting connection handler thread");

		thread([&browser] { broker(browser); }).detach();

		LOG("Listening for clients");
	}

	return 0;
}
