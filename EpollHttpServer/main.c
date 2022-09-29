#include"EpollServer.h"

int main()
{
	int sfd = Init(9999);
	EpollRun(sfd);
	return 0;
}