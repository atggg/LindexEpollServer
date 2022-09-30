#pragma once

typedef struct ClientSendInfo {
	int cfd;
	char* msg;
}ClientSendInfo;

//记录文件描述符的文件下载位置 
typedef struct TcpFilePos
{
	int cfd;
	char* fileName;
	unsigned int pos;
}TcpFilePos;

typedef struct PosList
{
	TcpFilePos* val;
	struct PosList* next;
}PosList;


int Init(int port);
int EpollRun(int sfd);
int ConnectClient(int sfd, int epfd);

void* http_request(void* arg);

void SendHttpHead(int cfd, int state, const char* stateStr, const char* type, int len);

const char* getFiletype(const char* name);

void Send404(int cfd);

void SendDir(int cfd,const char *dir);
void SendFile(int cfd, const char* fileName, unsigned long satrtOffet, unsigned long endOffet);
void* TSendFile(void* arg);


void ListInsert(PosList** head,const TcpFilePos* val);
TcpFilePos *ListFind(PosList** head, int cfd);
void ListDel(PosList** head, int cfd);
int ListCount(PosList** head);