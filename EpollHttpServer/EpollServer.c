#include "EpollServer.h"
#include<sys/socket.h>
#include<arpa/inet.h>
#include<sys/epoll.h>
#include<fcntl.h>
#include<malloc.h>
#include<errno.h>
#include<string.h>
#include<pthread.h>
#include<stdio.h>
#include<stdlib.h>
#include<sys/stat.h>
#include<dirent.h>
#include<assert.h>
#include<sys/types.h>
#include<unistd.h>

//buff大小
#define BUFFSIZE 10240

//锁
pthread_mutex_t mtx;
//链表的头
PosList* ListHead = NULL;

//初始化本地套接字
int Init(int port)
{
    //创建服务端socket
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd == -1)
    {
        perror("socket");
        return -1;
       
    }
    //设置复用
    int opt = 1;
    int rc = setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (rc == -1)
    {
        perror("setsockopt");
        return -1;
    }

    struct sockaddr_in saddr;
    saddr.sin_addr.s_addr = 0; //设置ip地址
    saddr.sin_family = AF_INET; //设置协议
    saddr.sin_port = htons(port);//设置端口号


    //绑定本地端口
    rc = bind(sfd, (struct sockaddr*)&saddr, sizeof(saddr));
    if (rc == -1)
    {
        perror("bind");
        return -1;
    }

    //设置监听
    rc = listen(sfd, 128);
    if (rc == -1)
    {
        perror("listen");
        return -1;
    }

    return sfd;
}


int EpollRun(int sfd)
{
    //创建一个Epoll树
    int epfd = epoll_create(1);
    if (epfd == -1)
    {
        perror("epoll_create");
        return -1;
    }
    //把服务器端的套接字文件描述符添加到epoll树上 让内核帮我们管理

    struct epoll_event ev;
    ev.data.fd = sfd;
    ev.events = EPOLLIN ;//EPOLLIN代表读事件 
    int rc = epoll_ctl(epfd, EPOLL_CTL_ADD, sfd, &ev);
    if (rc == -1)
    {
        perror("epoll_create");
        return -1;
    }
    //进入主循环
    while (1)
    {
        //开始监听epoll树
        struct epoll_event evs[1024] = {0};
        int num = epoll_wait(epfd, evs, 1024, 1);

        //遍历epoll树
        for (int i = 0; i < num; i++)
        {
            int fd = evs[i].data.fd;
            int events = evs[i].events;
            //判断这个文件描述夫是不是服务器监听的文件描述符号 如果是那就和客户端建立连接 
            if (fd == sfd)
            {
                ConnectClient(sfd, epfd);
            }
            //如果不是服务器监听的文件描述符 那一定是通信的文件描述符了
            //读事件
            if(fd != sfd && events & EPOLLIN)
            {
                printf("读事件触发 fd:%d\n",fd);
                //事情内存接收客户端发来的数据
                char* buffs = (char *)malloc(BUFFSIZE);
                int pos = 0;
                //循环接收
                while (1)
                {
                    int len = recv(fd, buffs+pos, 1024, 0);
                    pos += len;
                    //如果==0 那么就是客户端关闭了连接
                    if (len == 0)
                    {
                        //从读epoll树删除
                        rc = epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                        //关闭连接
                        close(fd);
                        pthread_mutex_lock(&mtx);
                        ListDel(&ListHead, fd);
                        printf("客户端 %d跑路了 从链表删除保存未发完全的文件记录 当前列表元素个数:%d\n",fd, ListCount(&ListHead));
                        pthread_mutex_unlock(&mtx);
                        break;
                    }
                    //如果是-1 并且 errno= EAGAIN 那就读取完成了
                    if (len == -1 && errno == EAGAIN)
                    {
                        ClientSendInfo* info = (ClientSendInfo*)malloc(sizeof(ClientSendInfo));
                        info->cfd = fd;
                        info->msg = (char*)malloc(strlen(buffs)+1);
                        memcpy(info->msg, buffs, strlen(buffs) + 1);
                        pthread_t th;
                        pthread_create(&th, NULL, HttpRequest, info);
                        break;
                    }
                    //这个情况就是发生错误了
                    if (len == -1 && errno != EAGAIN)
                    {
                        perror("recv");
                        break;
                    }
                }
                //释放内存
                free(buffs);
            }
            if (fd != sfd && events & EPOLLOUT)
            {
                printf("写事件触发 fd:%d\n", fd);
                pthread_mutex_lock(&mtx);
                TcpFilePos* filePos = ListFind(&ListHead, fd);
                pthread_mutex_unlock(&mtx);
                if (filePos != NULL)
                {
                    pthread_t t;
                    printf("创建一个线程去处理没有发送完的数据\n");
                    pthread_create(&t, NULL, TSendFile, (void*)fd);
                }
            }
        }
    }

    return 0;
}
//和客户端建立连接 
int ConnectClient(int sfd, int repfd)
{
    struct sockaddr_in caddr;
    //和客户端建立连接 
    socklen_t csize = (socklen_t)sizeof(caddr);
    int cfd =  accept(sfd, (struct sockaddr*)&caddr,& csize);
    if (cfd == -1)
    {
        perror("accept");
        return -1;
    }
    printf("收到一个连接 ip:%s 端口:%d 描述符:%d\n", inet_ntoa(caddr.sin_addr), caddr.sin_port, cfd);

    //把通信的描述符设置成非阻塞
    int flag = fcntl(cfd, F_GETFL);
    flag |= O_NONBLOCK;
    fcntl(cfd, F_SETFL, flag);

    //把建立的客户端连接添加到Epoll树上
    struct epoll_event ev;
    ev.data.fd = cfd;
    ev.events = EPOLLIN | EPOLLET | EPOLLOUT;
    int rc = epoll_ctl(repfd, EPOLL_CTL_ADD,cfd, &ev);
    if (rc == -1)
    {
        perror("epoll_ctl");
        return -1;
    }
    printf("处理客户端连接完成!\n");
    return 0;
}
//处理http请求
void* HttpRequest(void* arg)
{
    ClientSendInfo* info = (ClientSendInfo*)arg;
    char method[1000] = { 0 };//请求类型
    char path[1024] = { 0 };//可能包含url编码的路径
    char paths[1024] = { 0 };//已经解码的路径
    char protocol[100] = { 0 };//http版本
    char line[1050] = { 0 }; //请求行
    int rangeFlag = 0;
    unsigned int startOffet = 0;//文件开始的偏移量 主要是支持多线程下载和浏览器播放mp4
    unsigned int endOffet = 0;//文件结束偏移量 主要是支持多线程下载和浏览器播放mp4

    printf(info->msg);

    //处理请求行信息
    sscanf(info->msg, "%[^\r\n]", line); //从消息内取出请求行；
    sscanf(line, "%[^ ] %[^ ] %[^ ]", method, path, protocol); //从请求行内取出请求类型和路径
    printf("method = %s, path = %s, protocol = %s\n", method, path, protocol);
    //处理url编码
    decode_str(paths, path);


    //处理请求文件偏移量
    char* pos = strstr(info->msg, "\r\n\r\n");
    if (pos != NULL)
    {
        pos[0] = '\0';
    }
    char *s1 = strstr(info->msg, "Range");
    if (s1 != NULL)
    {
        rangeFlag = 1;
        s1 = strstr(s1, "bytes=");
        if (s1 != NULL)
        {
            printf("s1:%s\n",s1);
            s1 += 6;
            char* s2 = strstr(s1,"-");
            if (s2 != NULL)
            {
                s2[0] = '\0';
                startOffet = atoi(s1);
                s2[0] = '-';
            }
        }
        s1 = strstr(s1, "-");
        if (s1 != NULL)
        {
            printf("s1:%s\n", s1);
            s1 += 1;
            char* s2 = strstr(s1, "\r\n");
            if (s2 != NULL)
            {
                s2[0] = '\0';
                endOffet = atoi(s1);
                s2[0] = '-';
            }
        }
    }
    if (pos != NULL)
    {
        pos[0] = '\r';
    }
    printf("文件开始偏移量:%d  文件结束偏移量:%d Range标志:%d\n", startOffet,endOffet,rangeFlag);
    if (strcmp(method,"GET")!=0)
    {
        //如果不是GET请求 那就直接回复错误
        printf("不是GET请求直接回复404\n");
        Send404(info->cfd);
    }
    else
    {
        //处理GET 请求
        char* file = NULL;
        //判断是不是请求服务器的根目录
        if (strcmp(paths, "/") == 0)
        {
            file = "./";
        }
        else
        {
            //比如 path = /666/1.jpg 那么file就是 666/1.jpg 了 
            file = paths + 1;
        }
        //判断请求的路径是否存在
        struct stat sa;
        int rc = stat(file, &sa);
        if (rc == -1)
        {
            Send404(info->cfd);

        }
        else
        {
            //判断是文件还是路径
            if (S_ISDIR(sa.st_mode))
            {
                //是目录 给客户端回复目录的消息
                SendDir(info->cfd, file);
            }
            else
            {
                //是文件 给客户端发生文件
                SendHttpHead(info->cfd, 200, "OK", getFiletype(file), sa.st_size);
                SendFile(info->cfd, file, startOffet,0);
            }
        }
        
    }
    free(info->msg);
    free(info);
    return NULL;
}

void SendHttpHead(int cfd, int state, const char* stateStr, const char* type, int len)
{
    char buf[1024] = { 0 };
    // 状态行
    sprintf(buf, "http/1.1 %d %s\r\n", state, stateStr);
    send(cfd, buf, strlen(buf), MSG_NOSIGNAL);
    // 消息报头
    sprintf(buf, "Content-Type: %s\r\n", type);
    sprintf(buf+strlen(buf), "Content-Length: %ld\r\n", len);
    send(cfd, buf, strlen(buf), MSG_NOSIGNAL);
    // 空行
    send(cfd, "\r\n", 2, MSG_NOSIGNAL);
}

void Send404(int cfd)
{
    char szText[] = "<html><head><title>404</title></head>\n<body bgcolor=\"#cc99cc\"><h2 align=\"center\">Not Found</h4>\n<hr>\n</body>\n</html>\n";
    SendHttpHead(cfd, 404, "", ".html", sizeof(szText));
    send(cfd, szText, sizeof(szText), MSG_NOSIGNAL);
}



//发送目录
void SendDir(int cfd, const char* dir)
{
    printf("发送目录\n");
    //组装一个目录的html界面
    char* html = (char*)malloc(BUFFSIZE);
    memset(html, 0, BUFFSIZE);
    sprintf(html, "<html><head><title>%s</title></head><body><table>", dir);
    struct dirent** fileList;
    int num = scandir(dir, &fileList, NULL, alphasort);
    for (int i = 0; i < num; i++)
    {
        struct stat sa;
        char filePath[1024] = { 0 };
        sprintf(filePath, "%s/%s", dir, fileList[i]->d_name);
        stat(filePath, &sa);
        if (S_ISDIR(sa.st_mode))
        {
            //如果是目录
            sprintf(html + strlen(html), "<tr><td><a href=\"%s/\">%s</a></td><td></td><td>DIR</td></tr>", fileList[i]->d_name, fileList[i]->d_name);
        }
        else
        {
            //如果是文件
            sprintf(html + strlen(html), "<tr><td><a href=\"%s\">%s</a></td><td>%ld</td><td>FILE</td></tr>", fileList[i]->d_name, fileList[i]->d_name, sa.st_size);
        }
        free(fileList[i]);
    }
    sprintf(html + strlen(html), "</table></body></html>");
    free(fileList);
    //开始发送
    SendHttpHead(cfd, 200, "OK", getFiletype(".html"), strlen(html));
    send(cfd, html, strlen(html), MSG_NOSIGNAL);
    free(html);
}
//发送文件
void SendFile(int cfd, const char* fileName ,unsigned long startOffet, unsigned long endOffet)
{
    
    int ffd = open(fileName, O_RDONLY);
    assert(ffd > 0);
    if (endOffet == 0)
    {
        endOffet = lseek(ffd, 0, SEEK_END);
    }
    lseek(ffd, (off_t)startOffet, SEEK_SET);
    char* buff = (char*)malloc(BUFFSIZE);
    unsigned int pos = startOffet;
    while (endOffet > pos)
    {
        //读数据 判断结束位置-当前位置 是否>=buff的最大容量 如果成立 那就采用buff的最大容量 不如不成立 那就代表 快到快到结束的位置了 不需要用buff的最大容量了 
        int rlen = read(ffd, buff, endOffet - pos >= BUFFSIZE ? BUFFSIZE : endOffet - pos);
        if (rlen > 0)
        {
            int wlen = send(cfd, buff, rlen, MSG_NOSIGNAL);
            if (wlen <= 0)
            {
                printf("客户端关闭了流\n");
                TcpFilePos filePos;
                filePos.cfd = cfd;
                filePos.fileName = fileName;
                filePos.pos = pos;
                //保存当前文件的的位置
                pthread_mutex_lock(&mtx);
                //有可能这个连接已经关闭了 所以再判断一下
                char buf[1] = {0};
                int rc = recv(cfd, buf, 1, 0);
                //这种情况就是客户端没有断开连接只是断开了流
                if (rc == -1 && errno == EAGAIN)
                {
                    ListInsert(&ListHead, &filePos);
                    printf("保存当前文件的的位置\n");
                    TcpFilePos* test = ListFind(&ListHead, cfd);
                    printf("文件名字:%s 文件位置:%d 通信描述符号:%d 当前列表元素个数:%d\n", test->fileName, test->pos, test->cfd, ListCount(&ListHead));
                }
                pthread_mutex_unlock(&mtx);
                break;
            }
            pos += wlen;
            usleep(10);
        }
        else if (rlen == 0)
        {
            printf("文件正常发送完成\n");
            break;
        }
        else
        {
            perror("read");
            break;
        }
    }
    close(ffd);
    free(buff);
}
//主要用用于继续发送
void* TSendFile(void* arg)
{
    pthread_mutex_lock(&mtx);
    TcpFilePos* filePos = ListFind(&ListHead, (int)arg);
    if (filePos == NULL)
    {
        //上步是查询 这一步判断是方式进入到这个线程后防止filepos已经被删除了
        //如果已经被删除了那就不用继续了
        //这里锁的主要作用是保证位置信息的安全拷贝 
        pthread_mutex_unlock(&mtx);
        return;
    }
    int cfd = filePos->cfd;
    unsigned int pos = filePos->pos;
    char fileName[1024] = { 0 };
    memcpy(fileName, filePos->fileName, strlen(filePos->fileName) + 1);
    ListDel(&ListHead, filePos->cfd);
    printf("已经把链表的数据取出并且删除消息 当前列表元素个数:%d\n", ListCount(&ListHead));
    pthread_mutex_unlock(&mtx);
    SendFile(cfd, fileName, pos,0);
}

void ListInsert(PosList** head, const TcpFilePos* val)
{
    PosList* l1 = (PosList*)malloc(sizeof(PosList));
    l1->next = NULL;
    l1->val = (TcpFilePos*)malloc(sizeof(TcpFilePos));
    l1->val->cfd = val->cfd;
    l1->val->pos = val->pos;
    l1->val->fileName = (char*)malloc(strlen(val->fileName) + 1);
    memcpy(l1->val->fileName, val->fileName, strlen(val->fileName) + 1);
    if (*head == NULL)
    {
        *head = l1;
        return;
    }
    else
    {
        l1->next = *head;
        *head = l1;
        return;
    }
}

TcpFilePos *ListFind(PosList** head, int cfd)
{
    PosList* l = *head;
    if (l == NULL)
    {
        return NULL;
    }
    do
    {
        if (l->val->cfd == cfd)
        {
            return l->val;
        }
    } while (l=l->next);
    return NULL;
}

void ListDel(PosList** head, int cfd)
{
    PosList* l = *head;
    PosList* s = NULL;
    if (l == NULL)
    {
        return;
    }
    do
    {
        if (l->val->cfd == cfd && s == NULL)
        {
            *head = l->next;
            free(l->val->fileName);
            free(l->val);
            free(l);
            return;
        }
        else if (l->val->cfd == cfd && s != NULL)
        {
            s->next = l->next;
            free(l->val->fileName);
            free(l->val);
            free(l);
            return;
        }
        else
        {
            s = l;
        }
    } while (l=l->next);
}

int ListCount(PosList** head)
{
    int count = 0;
    PosList* l = *head;
    if (*head == NULL || head == NULL)
    {
        return count;
    }
    do
    {
        count++;
    } while (l=l->next);
    return count;
}



// 通过文件名获取文件的类型
const char* getFiletype(const char* name)
{
    char* dot;

    // 自右向左查找‘.’字符, 如不存在返回NULL
    dot = strrchr(name, '.');
    if (dot == NULL)
        return "text/plain; charset=utf-8";
    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0)
        return "text/html; charset=utf-8";
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0)
        return "image/jpeg";
    if (strcmp(dot, ".gif") == 0)
        return "image/gif";
    if (strcmp(dot, ".png") == 0)
        return "image/png";
    if (strcmp(dot, ".css") == 0)
        return "text/css";
    if (strcmp(dot, ".au") == 0)
        return "audio/basic";
    if (strcmp(dot, ".wav") == 0)
        return "audio/wav";
    if (strcmp(dot, ".avi") == 0)
        return "video/x-msvideo";
    if (strcmp(dot, ".mov") == 0 || strcmp(dot, ".qt") == 0)
        return "video/quicktime";
    if (strcmp(dot, ".mpeg") == 0 || strcmp(dot, ".mpe") == 0)
        return "video/mpeg";
    if (strcmp(dot, ".vrml") == 0 || strcmp(dot, ".wrl") == 0)
        return "model/vrml";
    if (strcmp(dot, ".midi") == 0 || strcmp(dot, ".mid") == 0)
        return "audio/midi";
    if (strcmp(dot, ".mp3") == 0)
        return "audio/mpeg";
    if (strcmp(dot, ".mp4") == 0 || strcmp(dot, ".m4a") == 0)
        return "audio/mp4";
    if (strcmp(dot, ".ogg") == 0)
        return "application/ogg";
    if (strcmp(dot, ".pac") == 0)
        return "application/x-ns-proxy-autoconfig";

    return "text/plain; charset=utf-8";
}


// 16进制数转化为10进制
int hexit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;

    return 0;
}

/*
 *  这里的内容是处理%20之类的东西！是"解码"过程。
 *  %20 URL编码中的‘ ’(space)
 *  %21 '!' %22 '"' %23 '#' %24 '$'
 *  %25 '%' %26 '&' %27 ''' %28 '('......
 *  相关知识html中的‘ ’(space)是&nbsp
 */
void encode_str(char* to, int tosize, const char* from)
{
    int tolen;

    for (tolen = 0; *from != '\0' && tolen + 4 < tosize; ++from) {
        if (isalnum(*from) || strchr("/_.-~", *from) != (char*)0) {
            *to = *from;
            ++to;
            ++tolen;
        }
        else {
            sprintf(to, "%%%02x", (int)*from & 0xff);
            to += 3;
            tolen += 3;
        }
    }
    *to = '\0';
}

void decode_str(char* to, char* from)
{
    for (; *from != '\0'; ++to, ++from) {
        if (from[0] == '%' && isxdigit(from[1]) && isxdigit(from[2])) {
            *to = hexit(from[1]) * 16 + hexit(from[2]);
            from += 2;
        }
        else {
            *to = *from;
        }
    }
    *to = '\0';
}