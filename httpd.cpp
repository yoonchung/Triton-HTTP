#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <iostream>
#include <map>
#include <algorithm>
#include <utility>
#include <string>
#include <sys/sendfile.h>
#include "httpd.h"

using namespace std;

const int BUFSIZE = 8192;
static const int MAXPENDING = 5;
const string SPACE = " ";
const string CRLF = "\r\n";
const string CRLFCRLF = "\r\n\r\n";
const string GETMETHOD = "GET";
const string HTTP11 = "HTTP/1.1";
const string ROOT = "/";
const string SERVER = "www.cs.ucsd.edu";

struct ThreadArgs {
  int clntSock; // Socket descriptor for client
  string doc_root;
};

void start_httpd(unsigned short port, string doc_root)
{
  cerr << "Starting server (port: " << port <<
  ", doc_root: " << doc_root << ")" << endl;
  
  /* Handle command line arguments */
  /* Create network socket */
  in_port_t servPort = port;

  // Create socket for incoming connections
  int servSock; // Socket descriptor for server
  if ((servSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
    DieWithSystemMessage("Socket faied");

  // Construct local address structure
  struct sockaddr_in servAddr;                  // Local address
  memset(&servAddr, 0, sizeof(servAddr));       // Zero out structure
  servAddr.sin_family = AF_INET;                // IPv4 address family
  servAddr.sin_addr.s_addr = htonl(INADDR_ANY); // Any incoming interface
  servAddr.sin_port = htons(servPort);          // Local port

  /* Bind socket to an interface */
  // Bind to the local address
  if (bind(servSock, (struct sockaddr*) &servAddr, sizeof(servAddr)) < 0)
    DieWithSystemMessage("bind() failed");

  /* Tell the socket to listen for incoming connections */
  // Mark the socket so it will listen for incoming connections
  if (listen(servSock, MAXPENDING) < 0)
    DieWithSystemMessage("listen() failed");

  for (;;) { // Run forever
    struct sockaddr_in clntAddr; // Client address
    // Set length of client address structure (in-out parameter)
    socklen_t clntAddrLen = sizeof(clntAddr);

    // Wait for a client to connect
    int clntSock = accept(servSock, (struct sockaddr *) &clntAddr, &clntAddrLen);
    if (clntSock < 0)
      DieWithSystemMessage("accept() failed");

    // clntSock is connected to a client!
    char clntName[INET_ADDRSTRLEN]; // String to contain client address
    if (inet_ntop(AF_INET, &clntAddr.sin_addr.s_addr, clntName,
        sizeof(clntName)) != NULL)
      printf("Handling client %s/%d\n", clntName, ntohs(clntAddr.sin_port));
    else
      puts("Unable to get client address");

    // Create separate memory for client argument
    struct ThreadArgs * threadArgs;
    threadArgs = (struct ThreadArgs *) malloc(sizeof(struct ThreadArgs));
    if (threadArgs == NULL)
      DieWithSystemMessage("malloc() failed");
    threadArgs->clntSock = clntSock;
    threadArgs->doc_root = doc_root;

    // Create client thread
    pthread_t threadID;
    int retVal = pthread_create(&threadID, NULL, ThreadMain, threadArgs);
    if (retVal != 0)
      DieWithSystemMessage("pthread_create() failed");

    /* Read/write to the socket */
    // ThreadMain hadnles this function call
    // HandleTCPClient(clntSock, N);
  }
  // NOT REACHED
}

void *ThreadMain(void *threadArgs) {
  // Guarantees that thread resources are deallocated upon return
  pthread_detach(pthread_self());

  // Extract socket file descriptor from argument
  int clntSock = ((struct ThreadArgs *)threadArgs)->clntSock;
  string doc_root = ((struct ThreadArgs *)threadArgs)->doc_root;
  free(threadArgs); // Deallocate memory for argument

  HandleTCPClient(clntSock, doc_root);
  return NULL;
}

void sendAll(int clntSocket, char* buffer, ssize_t len) {
  ssize_t sent_cnt = 0;
  while (sent_cnt < len) {
    ssize_t numBytesSent = send(
        clntSocket, buffer + sent_cnt, len - sent_cnt, 0);
    if (numBytesSent < 0)
      DieWithSystemMessage("send() failed");
    else if (numBytesSent == 0)
      DieWithUserMessage("send()", "failed to send anything");
    sent_cnt += numBytesSent;
  }
}

HTTPMessage recvTillCRLF(int clntSocket, char* buffer, ssize_t buf_size) {
  HTTPMessage m;
  ssize_t recv_cnt = 0;
  printf("Prepare to recv.\n");
  while (true) {
    ssize_t recv_cnt_prev = recv_cnt;
    ssize_t numBytesRcvd = recv(
        clntSocket, buffer + recv_cnt, buf_size - recv_cnt, 0);
    recv_cnt += numBytesRcvd;
    for (ssize_t i = recv_cnt_prev; i < recv_cnt; ++i) {
      if (buffer[i] == '\r')
        if (buffer[i+1] == '\n')
          if (buffer[i+2] == '\r')
            if (buffer[i+3] == '\n') {
              buffer[i] = '\0';
              string str(buffer, i+1);
              // i being numBytesRcvd
              m.msg = (string)str;
              return m;
            }
    }
  }
  return m;
}

void HandleTCPClient(int clntSocket, string doc_root) {
  printf("Starting to receive client.\n");
  char buffer[BUFSIZE*4]; // Buffer for echo string
  string str;

  // Receive message from client
  HTTPMessage m = recvTillCRLF(clntSocket, buffer, BUFSIZE*4);
  if (m.msg.length() <= 0)
    DieWithSystemMessage("recv() failed..");
  // Send it back N times
  HTTPRequest req = parseRequest(m);
  HTTPResponse res = serveRequest(req, doc_root);

  string reqPath = doc_root.append(req.uri);
  char resPathp[1024];
  realpath(reqPath.c_str(), resPathp);
  std::string resPath(resPathp);

  returnResponse(clntSocket, res, resPath);
  close(clntSocket); // Close client socket
}

void DieWithUserMessage(const char *msg, const char *detail) {
  fputs(msg, stderr);
  fputs(": ", stderr);
  fputs(detail, stderr);
  fputc('\n', stderr);
  exit(1);
}

void DieWithSystemMessage(const char *msg) {
  perror(msg);
  exit(1);
}

HTTPRequest parseRequest(HTTPMessage m) {
  printf("Starting to parse user request\n");
  HTTPRequest req;
  size_t pos = 0, prev = 0, vpos = 0, vprev = 0;
  string token;
  string init[3];
  //pos < m.msg.find_first_of(CRLF)
  for (int i = 0; i < 2; ++i) {
    pos = m.msg.find_first_of(SPACE, prev);
    init[i] = m.msg.substr(prev, pos - prev);
    prev = pos + 1;
  }
  pos = m.msg.find_first_of("\r\n", prev);
  init[2] = m.msg.substr(prev, pos - prev);
  prev = pos + 2;
  req.method = init[0];
  req.uri = init[1];
  req.httpVer = init[2];
  vpos = pos;
  vprev = prev;
  
  do {
    pos = m.msg.find_first_of(" ", prev);
    vprev = pos + strlen(" ");
    vpos = m.msg.find_first_of("\r\n", vprev);
    req.kvpair.insert(pair<string, string>
      ( m.msg.substr(prev, pos - prev), m.msg.substr(vprev, vpos - vprev)) );
    prev = vpos + strlen("\r\n");
  } while (vpos != string::npos);
    for (map<string, string>::const_iterator it = req.kvpair.begin();
      it != req.kvpair.end(); ++it) {
  }
  return req;
}

HTTPResponse serveRequest(HTTPRequest req, string doc_root) {
  printf("Starting to serve user request\n");
  HTTPResponse res;
  string reqPath = doc_root.append(req.uri);
  
  // Test for 400 error
  if (req.method != GETMETHOD) {
    printf("\tExiting from method\n");
    res.httpVer = HTTP11;
    res.status = 400;
    res.retmsg = "Client Error";
    return res;
  }
  else if (req.httpVer != HTTP11) {
    printf("\tExiting from http version\n");
    res.httpVer = HTTP11;
    res.status = 400;
    res.retmsg = "Client Error";
    return res;
  }
  else {
    bool missingHost = true;
    bool missingColon = false;
    for (map<string, string>::const_iterator it = req.kvpair.begin(); 
      it != req.kvpair.end(); ++it) {
      if (it->first == "Host:")
        missingHost = false;
      if ((it->first.find(":")) == ((size_t)strlen(it->first.c_str()-1)))
        missingColon = true;
    }
    if (missingHost) {
      printf("\tExiting from host\n");
      res.httpVer = HTTP11;
      res.status = 400;
      res.retmsg = "Client Error";
      return res;
    }
    if (missingColon) {
      printf("\tExiting from missingColon\n");
      res.httpVer = HTTP11;
      res.status = 400;
      res.retmsg = "Client Error";
      return res;
    }
  }
  // Test for 404 error
  struct stat sb;
  char resPathp[1024];
  char *resl = realpath(reqPath.c_str(), resPathp);
  std::string resPath(resPathp);
  if (!resl) {
    printf("\tExiting from realpath\n");
    res.httpVer = HTTP11;
    res.status = 404;
    res.retmsg = "Not Found";
    return res;
  }
  if (std::count(resPath.begin(), resPath.end(), '/') 
      < std::count(doc_root.begin(), doc_root.end(), '/') && reqPath != ROOT) {
    printf("\tExiting from uri fault\n");
    res.httpVer = HTTP11;
    res.status = 404;
    res.retmsg = "Not Found";
    return res;
  }
  else {
    if (S_ISDIR(sb.st_mode))
      resPath += "\"index.html";
    req.realpath = resPath;
  }
  // File not available
  if (stat(resPath.c_str(), &sb) == -1) {
    printf("\tExiting from file not exist\n");
    res.httpVer = HTTP11;
    res.status = 404;
    res.retmsg = "Not Found";
    return res;
  }
  // No read permission
  else if (!(sb.st_mode & S_IROTH)) {
    printf("\tExiting from file permission\n");
    res.httpVer = HTTP11;
    res.status = 403;
    res.retmsg = "Forbidden";
  }
  else {
    int in_fd = open(resPath.c_str(), O_RDONLY);
    if (in_fd < 0)
      DieWithSystemMessage("Open failed");
    // Prepare response
    res.httpVer = HTTP11;
    res.status = 200;
    res.retmsg = "OK";
    req.in_fd = in_fd;
    res.kvpair.insert(pair<string, string>("Server: ", SERVER));

    // Getting last-modified
    char lastMod[1024];
    time_t t = sb.st_mtime;
    struct tm *gmt;
    gmt = gmtime(&t);
    strftime(lastMod, sizeof lastMod, "%a, %d %b %Y %H:%M:%S %Z", gmt);
    res.kvpair.insert(pair<string, string>("Last-Modified: ", lastMod));
    size_t pos = req.uri.find(".");
    string ext = req.uri.substr(pos+1, strlen(req.uri.c_str()) - pos-1);
    string type;
    if (ext == "jpg")
      type = "image/jpeg";
    if (ext == "html")
      type = "text/html";
    if (ext == "png")
      type = "image/png";
    res.kvpair.insert(pair<string, string>("Content-Type: ", type));
    // Getting content length
    string size = to_string(sb.st_size);

    res.kvpair.insert(pair<string, string>("Content-Length: ", size));
  }
  return res;
}

void returnResponse(int clntSocket, HTTPResponse res, string realpath) {
  // 
  printf("Starting to send server response\n\n");

  HTTPMessage m;
  m.msg.append(res.httpVer+" ");
  m.msg.append(to_string(res.status)+" ");
  m.msg.append(res.retmsg+"\r\n");
  for (map<string, string>::reverse_iterator it = res.kvpair.rbegin();
    it != res.kvpair.rend(); ++it) {
    m.msg.append(it->first);
    m.msg.append(it->second + "\r\n");
  }
  m.msg.append("\r\n");
  const char *buffer = m.msg.c_str();

  ssize_t len = strlen(buffer);
  ssize_t sent_cnt = 0, sentfile_cnt = 0;
  /*
  char buffer[sizeof (HTTPResponse)];
  memset(buffer, '\0', sizeof (HTTPResponse));
  memcpy(buffer, &res, sizeof (HTTPResponse));
  */
  while (sent_cnt < len) {
    ssize_t numBytesSent = send(
        clntSocket, buffer + sent_cnt, len - sent_cnt, 0);
    if (numBytesSent < 0)
      DieWithSystemMessage("send() failed");
    else if (numBytesSent == 0)
      DieWithUserMessage("send()", "failed to send anything");
    sent_cnt += numBytesSent;
  }
  if (res.status == 200) {
    off_t offset = 0;
    size_t length;
    struct stat sb;
    if (stat(realpath.c_str(), &sb) == -1) {
      perror("stat");
      exit(EXIT_FAILURE);
    }
    length = sb.st_size;
    int in_fd = open(realpath.c_str(), O_RDONLY);
    if (in_fd < 0)
      DieWithSystemMessage("Open failed");
    while (sentfile_cnt < (ssize_t)length) {
      ssize_t numBytesSent = sendfile(
          clntSocket, in_fd, &offset, length);
      if (numBytesSent < 0)
        DieWithSystemMessage("sendfile() failed");
      else if (numBytesSent == 0)
        DieWithUserMessage("send()", "failed to send anything");
      sentfile_cnt += numBytesSent;
    }
  }
}
