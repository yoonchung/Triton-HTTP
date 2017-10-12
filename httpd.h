#ifndef HTTPD_H
#define HTTPD_H

#include <string>
#include <map>

using namespace std;

class HTTPMessage {
public:
  string msg;
  int length;
};

class HTTPRequest {
public:
  string method;
  string uri;
  string httpVer;
  int status;
  int in_fd;
  string realpath;
  std::map<string, string> kvpair;
};

class HTTPResponse {
public:
  string httpVer;
  int status;
  string retmsg;
  std::map<string, string> kvpair;
};

void start_httpd(unsigned short port, string doc_root);
void HandleTCPClient(int clntSocket, string doc_root);
void DieWithUserMessage(const char *msg, const char *detail);
void DieWithSystemMessage(const char *msg);
void *ThreadMain(void *threadArgs);
HTTPRequest parseRequest(HTTPMessage m);
HTTPResponse serveRequest(HTTPRequest req, string doc_root);
void returnResponse(int clntSocket, HTTPResponse res, string realpath);

#endif // HTTPD_H
