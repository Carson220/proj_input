/***************************************************************
*   文件名称：monitor.c
*   描    述：用于监听数据库键值变化并通知相应SDN控制器 
***************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "hiredis.h"
#include "async.h"
#include "adapters/libevent.h"

#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <byteswap.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <errno.h>
#include <pthread.h>
#include <netinet/tcp.h>
#include "db_wr.h"

#define MAX_NUM 66
#define SLOT_NUM 44
#define DB_NUM 6
#define CMD_MAX_LENGHT 256
// #define REDIS_SERVER_IP "192.168.10.118"

#define REDIS_SERVER_PORT 6379

// #define DB_ID 2 // database_id = 192.168.68.2
// #define SERVER_IP "127.0.0.1" // tcp+udp ip
#define SERVER_PORT 2345 // tcp port
#define TCP_SERVER_PORT 2346 // keep alive tcp port
#define UDP_PORT 12000 // udp port
#define BUFSIZE 512
#define ROUTE_ADD 1 // type_1 add
#define ROUTE_DEL 2 // type_2 del
#define CAL_FAIL 0
#define CAL_SUCCESS 1
#define GOTO_TABLE 255
#define IP_LEN 8
#define MAX_DIST 0x3f3f3f3f

int fd_ctl[MAX_NUM] = {0, }; // 记录不同控制器节点对应的套接字描述符
int slot = 0; // slot_id
int fail_link_index[SLOT_NUM][MAX_NUM] = {0, }; // 记录已经处理到的fail_link列表的索引
int server_fd_udp = -1; // UDP监听的套接字
int server_fd_tcp = -1; // TCP监听的套接字（数据库之间）

int keep_alive_flag[MAX_NUM] = {0, }; // flag=1 表示DB之间连接正常
int fd_db_client[MAX_NUM] = {0, }; // 记录不同数据库（客户端）对应的套接字描述符
int fd_db_server[MAX_NUM] = {0, }; // 记录不同数据库（服务端）对应的套接字描述符
int db_neighbor_list[DB_NUM-1] = {0, }; // 记录数据库邻居序号（IP-1）

void print_err(char *str, int line, int err_no) {
	printf("%d, %s :%s\n",line,str,strerror(err_no));
	// _exit(-1);
}

// udp 监听套接字初始化
int listen_init(char *redis_ip)
{
    // 初始化监听套接字
    int ret;
    struct sockaddr_in ser_addr;

    server_fd_udp = socket(AF_INET, SOCK_DGRAM, 0); //AF_INET:IPV4;SOCK_DGRAM:UDP
    if(server_fd_udp < 0)
    {
        printf("create socket fail!\n");
        return -1;
    }

    memset(&ser_addr, 0, sizeof(ser_addr));
    ser_addr.sin_family = AF_INET;
    ser_addr.sin_addr.s_addr = inet_addr(redis_ip); //IP地址，需要进行网络序转换，INADDR_ANY：本地地址
    ser_addr.sin_port = htons(UDP_PORT);  //端口号，需要网络序转换

    ret = bind(server_fd_udp, (struct sockaddr*)&ser_addr, sizeof(ser_addr));
    if(ret < 0)
    {
        printf("socket bind fail!\n");
        close(server_fd_udp);
        return -1;
    }
    return 0;
}

// tcp 子线程，等待客户端（控制器）连接
void *tcpconnect_ctl(void *pth_arg)
{
	long skfd = (long)pth_arg;

    // 使用accept阻塞形式得监听客户端的发来的连接，并返回通信描述符
	long cfd = -1;
    int ctrl_id = -1;
	pthread_t id;
    int keepAlive = 1; // 开启keepalive属性
    int keepIdle = 1; // 如该连接在1秒内没有任何数据往来,则进行探测 
    int keepInterval = 1; // 探测时发包的时间间隔为1 秒
    int keepCount = 2; // 探测尝试的次数

	while(1) 
    {
		struct sockaddr_in caddr = {0};
		int csize = sizeof(caddr);
		cfd = accept(skfd, (struct sockaddr*)&caddr, &csize);
        setsockopt(cfd, SOL_SOCKET, SO_KEEPALIVE, (void *)&keepAlive, sizeof(keepAlive));
        setsockopt(cfd, SOL_TCP, TCP_KEEPIDLE, (void*)&keepIdle, sizeof(keepIdle));
        setsockopt(cfd, SOL_TCP, TCP_KEEPINTVL, (void *)&keepInterval, sizeof(keepInterval));
        setsockopt(cfd, SOL_TCP, TCP_KEEPCNT, (void *)&keepCount, sizeof(keepCount));
		if (cfd == -1) 
        {
			print_err("accept failed", __LINE__, errno);
		}
		// 建立连接后打印一下客户端的ip和端口号
		printf("ctl: cport = %d, caddr = %s\n", ntohs(caddr.sin_port),inet_ntoa(caddr.sin_addr));
        // printf("ctrl_id = %d\n", ((inet_addr(inet_ntoa(caddr.sin_addr)))&0xff000000)>>24);
        // 记录SDN控制器对应的套接字描述符
        ctrl_id = (((inet_addr(inet_ntoa(caddr.sin_addr)))&0xff000000)>>24) -1;
        if(fd_ctl[ctrl_id] != 0) close(fd_ctl[ctrl_id]);
        fd_ctl[ctrl_id] = cfd;
	}
}

// tcp 子线程，等待客户端（数据库）连接
void *tcpconnect_db(void *pth_arg)
{
    // 使用accept阻塞形式得监听客户端的发来的连接，并返回通信描述符
	long cfd = -1;
    int db_id = -1;
	pthread_t id;
    int keepAlive = 1; // 开启keepalive属性
    int keepIdle = 1; // 如该连接在1秒内没有任何数据往来,则进行探测 
    int keepInterval = 1; // 探测时发包的时间间隔为1 秒
    int keepCount = 2; // 探测尝试的次数

	while(1) 
    {
		struct sockaddr_in caddr = {0};
		int csize = sizeof(caddr);
		cfd = accept(server_fd_tcp, (struct sockaddr*)&caddr, &csize);
        setsockopt(cfd, SOL_SOCKET, SO_KEEPALIVE, (void *)&keepAlive, sizeof(keepAlive));
        setsockopt(cfd, SOL_TCP, TCP_KEEPIDLE, (void*)&keepIdle, sizeof(keepIdle));
        setsockopt(cfd, SOL_TCP, TCP_KEEPINTVL, (void *)&keepInterval, sizeof(keepInterval));
        setsockopt(cfd, SOL_TCP, TCP_KEEPCNT, (void *)&keepCount, sizeof(keepCount));
		if (cfd == -1) 
        {
			print_err("accept failed", __LINE__, errno);
		}
		// 建立连接后打印一下客户端的ip和端口号
		printf("db: cport = %d, caddr = %s\n", ntohs(caddr.sin_port),inet_ntoa(caddr.sin_addr));
        // printf("ctrl_id = %d\n", ((inet_addr(inet_ntoa(caddr.sin_addr)))&0xff000000)>>24);
        // 记录数据库对应的套接字描述符
        db_id = (((inet_addr(inet_ntoa(caddr.sin_addr)))&0xff000000)>>24) -1;
        if(fd_db_client[db_id] != 0) close(fd_db_client[db_id]);
        fd_db_client[db_id] = cfd;
	}
}

// tcp 子线程，主动连接服务端（数据库）
void *tcpconnect_client(void *pth_arg)
{
    int ret = 0;
    int skfd = -1;
    struct sockaddr_in addr;
    char proxy_ip[20] = "192.168.68.";  // 数据库代理ip
    long i = (long)pth_arg;

    int keepAlive = 1; // 开启keepalive属性
    int keepIdle = 1; // 如该连接在1秒内没有任何数据往来,则进行探测 
    int keepInterval = 1; // 探测时发包的时间间隔为1 秒
    int keepCount = 2; // 探测尝试的次数
	
    skfd = socket(AF_INET, SOCK_STREAM, 0);
    if (skfd == -1) 
    {
        print_err("socket failed",__LINE__,errno);
    }
    addr.sin_family = AF_INET; //设置tcp协议族
    addr.sin_port = htons(TCP_SERVER_PORT); // 设置端口号
    sprintf(&proxy_ip[11], "%d", db_neighbor_list[i]+1);
    addr.sin_addr.s_addr = inet_addr(proxy_ip); //设置ip地址

    //主动发送连接请求
    while(connect(skfd, (struct sockaddr*)&addr, sizeof(addr)) == -1)
    {
        print_err("connect failed",__LINE__,errno);
        sleep(1);
    }

    setsockopt(skfd, SOL_SOCKET, SO_KEEPALIVE, (void *)&keepAlive, sizeof(keepAlive));
    setsockopt(skfd, SOL_TCP, TCP_KEEPIDLE, (void*)&keepIdle, sizeof(keepIdle));
    setsockopt(skfd, SOL_TCP, TCP_KEEPINTVL, (void *)&keepInterval, sizeof(keepInterval));
    setsockopt(skfd, SOL_TCP, TCP_KEEPCNT, (void *)&keepCount, sizeof(keepCount));
    fd_db_server[db_neighbor_list[i]] = skfd;
    keep_alive_flag[db_neighbor_list[i]] = 1;

}

// 利用主动连接其他数据库（服务端）的套接字
// 测试数据库之间的TCP持续连接情况
void *keep_alive(void *pth_arg)
{
    long i = 0;
    int j = 0;
    int skfd = -1;
    int maxfd = -1;
    int ret = -1;
    int ret_recv = -1;
    char buf[BUFSIZE] = {0};
    pthread_t tcpid;
    int num = 0; // 记录数据库邻居数量

    struct timeval timeout = {2,0}; // wait 2s
    fd_set read_fds;
    
    while(1)
    {
        FD_ZERO(&read_fds);
        maxfd = -1;
        for(i = 0; i < DB_NUM-1; i++)
        {
            // select 监听连接状态，同时设置断线重连
            skfd = fd_db_server[db_neighbor_list[i]];
            if(skfd != -1)
            {
                FD_SET(skfd, &read_fds);
                if(skfd > maxfd) maxfd = skfd+1;
            }
        }

        if(maxfd != -1)
        {
            timeout.tv_sec = 2;
            timeout.tv_usec = 0;
            ret = select(maxfd, &read_fds, NULL, NULL, &timeout);
            if(ret == -1)
            {
                print_err("select error",__LINE__,errno);
                for(i = 0; i < DB_NUM-1; i++)
                {
                    printf("%d ", fd_db_server[db_neighbor_list[i]]);
                }
                printf("\n");
            }
            else if(ret == 0)
            {
                // print_err("select tiemout",__LINE__,errno);
            }
            else if(ret > 0)
            {
                for(i = 0; i < DB_NUM-1; i++)
                {
                    skfd = fd_db_server[db_neighbor_list[i]];
                    if(FD_ISSET(skfd, &read_fds))
                    {
                        memset(buf, 0, BUFSIZE);
                        ret_recv = recv(skfd, buf, BUFSIZE , 0);
                        if(ret_recv == 0 || ret_recv == -1)
                        {
                            // 表示数据库断开连接
                            printf("数据库 %d 断开连接\n", db_neighbor_list[i]);
                            keep_alive_flag[db_neighbor_list[i]] = 0;
                            fd_db_server[db_neighbor_list[i]] = -1;
                            close(skfd);
                            // 重新连接
                            ret = pthread_create(&tcpid, NULL, tcpconnect_client, (void*)i);
                            if (ret == -1) 
                            {
                                print_err("create tcpconnect_client failed", __LINE__, errno); 
                            }
                        }
                        else if(ret_recv > 0)
                        {
                            // 收到数据库邻接状态询问
                            if(strstr(buf, "request") != NULL)
                            {
                                // 回复邻居（数据库）个数
                                num = 0;
                                for(j = 0; j < DB_NUM-1; j++)
                                {
                                    if(keep_alive_flag[db_neighbor_list[j]] == 1)
                                        num++;
                                }

                                memset(buf, 0, BUFSIZE);
                                snprintf(buf, BUFSIZE, "%d", num);
                                printf("\t\t\tbuf:%s\n", buf);
                                ret = send(skfd, buf, BUFSIZE, 0);
                                if (ret == -1)
                                {
                                    print_err("\t\t\tsend keepalive info failed", __LINE__, errno);
                                }
                            }
                        }
                    }
                }
            }
        }
        else
            sleep(1);
    }
}

// 用于输出路径，并写入文件
void out(int node1, int node2, int *route, int *nextsw, int *hop)
{
    printf("node1 = %02d, node2 = %02d, hop = %d\n", node1, node2, *hop);
    // printf("*route = %p, *nextsw = %p\n", route, nextsw);
    if(*(route + node1*MAX_NUM + node2) == -1)
    {
        printf("route[%02d][%02d] = -1\n", node1, node2);
        return;
    }
    out(node1, *(route + node1*MAX_NUM + node2), route, nextsw, hop);
    nextsw[(*hop)++] = *(route + node1*MAX_NUM + node2);
    printf("nextsw[%d] = %d\n", (*hop)-1, *(route + node1*MAX_NUM + node2));
    out(*(route + node1*MAX_NUM + node2), node2, route, nextsw, hop);
}

// 用于十六进制字符串转数字
int strtoi(char *str, int size)
{
    int num = 0;
    int i = 0;
    for(i = 0; i < size; i++)
    {
        if(str[i]>='0' && str[i]<='9')
            num = num*16+(str[i]-'0');
        else if(str[i]>='A' && str[i]<='F')
            num = num*16+(str[i]-'A'+10);
        else if(str[i]>='a' && str[i]<='f')
            num = num*16+(str[i]-'a'+10);
    }
    return num;
}

// 向控制器发送路由通告
void route_notice(int db_id, int sw, char *ip_src, char *ip_dst, int port1, int port2, char *redis_ip)
{
    long cfd = -1;
    char buf[BUFSIZE] = {0};
    int ret = -1;
    // printf("sw:%u, port1:%u, port2:%u\n", sw, port1, port2);

    // 等待路由同步到各个数据库之后再下发
    sleep(5);
    cfd = fd_ctl[sw];

    // type:1,sw:3,ip_src:8,ip_dst:8,outport:3,port2:3(port2字段用于承载第二个出端口)
    memset(buf, 0, BUFSIZE);
    printf("\t\t\t源节点下发, db_id:%d, ctrl_id:%d, sw:%d, ip_src:%s, ip_dst:%s, port:%d, port2:%d\n", db_id, sw, sw, ip_src, ip_dst, port1, port2);
    snprintf(buf, BUFSIZE, "%d%03d%s%s%03d%03d", ROUTE_ADD, sw, ip_src, ip_dst, port1, port2);
    printf("\t\t\tbuf:%s\n",buf);
    ret = send(cfd, buf, BUFSIZE, 0);
    if (ret == -1)
    {
        print_err("\t\t\tsend route failed", __LINE__, errno);
        // 发送失败表示控制器断开连接，将 buf 存入对应的待执行结构 wait_exec_X
        Add_Wait_Exec(sw, buf, redis_ip);
    }

    free(ip_src);
    free(ip_dst);
}

// 用于d2d线程传参的结构体
struct para
{
    void *obj;
    void *redis_ip;
};

// 选择第三方数据库写入路由调整命令
int db_write_select(int target_id)
{
    int i = 0;
    int skfd = -1;
    int maxfd = -1;
    int ret = -1;
    int ret_recv = -1;
    char buf[BUFSIZE] = {0};
    char req[] = "request";
    pthread_t tcpid;
    fd_set read_fds;
    int num = 0; // 记录数据库邻居数量
    int max_num = 0;
    int db_id = -1; // 记录num最大的数据库id
    
    // 询问数据库邻接状态
    for(i = 0; i < DB_NUM-1; i++)
    {
        skfd = fd_db_client[db_neighbor_list[i]];
        if(keep_alive_flag[db_neighbor_list[i]] == 1)
        {
            memset(buf, 0, BUFSIZE);
            snprintf(buf, BUFSIZE, "%s", req);
            printf("\t\t\tbuf:%s\n",buf);
            ret = send(skfd, buf, BUFSIZE, 0);
            if (ret == -1)
            {
                print_err("\t\t\tsend keepalive info failed", __LINE__, errno);
            }
        }
    }
    
    while(1)
    {
        FD_ZERO(&read_fds);
        maxfd = -1;
        for(i = 0; i < DB_NUM-1; i++)
        {
            // 利用连接到本数据库的客户端套接字
            skfd = fd_db_client[db_neighbor_list[i]];
            if(skfd != -1)
            {
                FD_SET(skfd, &read_fds);
                if(skfd > maxfd) maxfd = skfd+1;
            }
        }
        
        if(maxfd != -1)
        {
            ret = select(maxfd, &read_fds, NULL, NULL, NULL); // 阻塞监听
            if(ret == -1)
            {
                print_err("select error",__LINE__,errno);
                for(i = 0; i < DB_NUM-1; i++)
                {
                    printf("%d ", fd_db_client[db_neighbor_list[i]]);
                }
                printf("\n");
            }
            else if(ret > 0)
            {
                max_num = 0;
                db_id = -1;
                for(i = 0; i < DB_NUM-1; i++)
                {
                    skfd = fd_db_client[db_neighbor_list[i]];
                    if(FD_ISSET(skfd, &read_fds))
                    {
                        memset(buf, 0, BUFSIZE);
                        ret_recv = recv(skfd, buf, BUFSIZE , 0);
                        if(ret_recv > 0)
                        {
                            // 收到数据库邻接状态回复
                            num = buf[0] - '0';
                            printf("数据库 %d 邻居数量：%d\n", db_neighbor_list[i], num);
                            // 不选对端数据库
                            if((num > max_num) && (db_neighbor_list[i] != target_id) && (keep_alive_flag[db_neighbor_list[i]] == 1))
                            {
                                max_num = num;
                                db_id = db_neighbor_list[i];
                            }
                        }
                    }
                }
                return db_id;
            }
        }
        else
            sleep(1);
    }
}

// 根据d2d路由表项，汇总并下发交换机流表到控制器
void *d2d_thread(void *arg)
{
    struct para *d2d_para;
    d2d_para = (struct para *)arg;
    char *obj = (*d2d_para).obj;
    char *redis_ip = (*d2d_para).redis_ip;
    int num = obj[IP_LEN*2+7] - '0';
    printf("\tobj = %s, redis_ip = %s, num = %d\n", obj, redis_ip, num);

    char cmd[CMD_MAX_LENGHT] = {0};
    redisContext *context;
    redisReply *reply;
    int i = 0;
    int ctrl_id = 0; // 记录控制器ID
    int db_id = 0;
    int sw, sw_pre = 999;
    int port = 999;
    long cfd = -1;
    int ret = -1;
    char buf[BUFSIZE] = {0};
    char ip_src[IP_LEN+1] = {0,};
    char ip_dst[IP_LEN+1] = {0,};
    int port2 = 999; // 对于d2d路由，源节点的该字段存储第二个出端口，非源节点的该字段存储入端口
    strncpy(ip_src, &obj[6], IP_LEN);
    strncpy(ip_dst, &obj[6 + IP_LEN], IP_LEN);
    printf("\tip_src = %s, ip_dst = %s\n", ip_src, ip_dst);
    int outport_src = 999; // 记录源节点的第一条路由的出端口

    // 等待各数据库收到相应的新增路由之后，再分发路由通告修改流表
    sleep(5);

    if(num == 1) // 直接下发
    {
        snprintf(cmd, CMD_MAX_LENGHT, "lrange %s 0 -1", obj);
        context = redisConnect(redis_ip, REDIS_SERVER_PORT);
        if (context->err)
        { 
            printf("\tError: %s\n", context->errstr);
            redisFree(context);
            return NULL;
        }
        // printf("connect redis server success\n");
        reply = (redisReply *)redisCommand(context, cmd);
        if (reply == NULL)
        {
            printf("\texecute command:%s failure\n", cmd);
            redisFree(context);
            return NULL;
        }
        if(reply->elements == 0) 
        {
            freeReplyObject(reply);
            redisFree(context);
            printf("\troute lookup failed\n");
            return NULL;
        }

        sw = atoi(reply->element[0]->str)/1000;
        port = atoi(reply->element[0]->str)%1000;
        sw_pre = sw;

        for(i = 1; i < reply->elements; i++)
        {
            sw = atoi(reply->element[i]->str)/1000;
            port = atoi(reply->element[i]->str)%1000;
            // printf("sw:%u, outport:%u\n", sw, port);
            ctrl_id = sw;
            db_id = Get_Ctrl_Conn_Db((uint32_t)ctrl_id, redis_ip);

            // 判断该出端口属于本区域交换机，向对应控制器发送通告
            // DB_ID = (((inet_addr(redis_ip))&0xff000000)>>24) - 1
            if(db_id == (((inet_addr(redis_ip))&0xff000000)>>24) - 1)
            {
                cfd = fd_ctl[ctrl_id]; 
                // printf("cfd:%ld\n", cfd);

                // 入端口
                port2 = sw_pre;
                // add route to routes set <-> link
                Add_Rt_Set((uint32_t)sw, (uint32_t)port, ip_src, ip_dst, num, redis_ip);

                // type:1,sw:3,ip_src:8,ip_dst:8,outport:3,port2:3
                memset(buf, 0, BUFSIZE);
                printf("\t\t非源节点下发, db_id:%d, ctrl_id:%d, sw:%d, ip_src:%s, ip_dst:%s, port:%d, port2:%d\n", db_id, ctrl_id, sw, ip_src, ip_dst, port, port2);
                snprintf(buf, BUFSIZE, "%d%03d%s%s%03d%03d", ROUTE_ADD, sw, ip_src, ip_dst, port, port2);
                printf("\t\tbuf:%s\n",buf);
                ret = send(cfd, buf, BUFSIZE, 0);
                if (ret == -1)
                {
                    print_err("\t\tsend route failed", __LINE__, errno);
                    // 发送失败表示控制器断开连接，将 buf 存入对应的待执行结构 wait_exec_X
                    Add_Wait_Exec(ctrl_id, buf, redis_ip);
                }
            }
            sw_pre = sw;
        }
    }
    else if(num == 2) // 对于源节点，整合两条路径的出端口一起下发
    {
        context = redisConnect(redis_ip, REDIS_SERVER_PORT);
        if (context->err)
        { 
            printf("\tError: %s\n", context->errstr);
            redisFree(context);
            return NULL;
        }
        // printf("connect redis server success\n");
        
        // obj[IP_LEN*2+7] = '1';
        // snprintf(cmd, CMD_MAX_LENGHT, "lrange %s 0 -1", obj);
        // reply = (redisReply *)redisCommand(context, cmd);
        // if (reply == NULL)
        // {
        //     printf("execute command:%s failure\n", cmd);
        //     redisFree(context);
        //     return NULL;
        // }
        // if(reply->elements == 0) 
        // {
        //     freeReplyObject(reply);
        //     redisFree(context);
        //     return NULL;
        // }
        // outport_src = atoi(reply->element[0]->str)%1000;
        // freeReplyObject(reply);

        obj[IP_LEN*2+7] = '2';
        snprintf(cmd, CMD_MAX_LENGHT, "lrange %s 0 -1", obj);
        reply = (redisReply *)redisCommand(context, cmd);
        if (reply == NULL)
        {
            printf("\texecute command:%s failure\n", cmd);
            redisFree(context);
            return NULL;
        }
        if(reply->elements == 0) 
        {
            freeReplyObject(reply);
            redisFree(context);
            return NULL;
        }

        sw = atoi(reply->element[0]->str)/1000;
        port = atoi(reply->element[0]->str)%1000;
        // printf("sw:%u, outport:%u\n", sw, port);
        // ctrl_id = sw;
        // db_id = Get_Ctrl_Conn_Db((uint32_t)ctrl_id, redis_ip);
        // // 判断该出端口属于本区域交换机，向对应控制器发送通告
        // // DB_ID = (((inet_addr(redis_ip))&0xff000000)>>24) - 1
        // if(db_id == (((inet_addr(redis_ip))&0xff000000)>>24) - 1)
        // {
        //     cfd = fd[ctrl_id]; 
        //     port2 = outport_src;
        //     // add route to routes set <-> link
        //     Add_Rt_Set((uint32_t)sw, (uint32_t)port, ip_src, ip_dst, num, redis_ip);
        //     // type:1,sw:3,ip_src:8,ip_dst:8,outport:3,port2:3(port2字段用于承载第二个出端口)
        //     memset(buf, 0, BUFSIZE);
        //     printf("源节点下发, db_id:%d, ctrl_id:%d, sw:%d, ip_src:%s, ip_dst:%s, port:%d, port2:%d\n", db_id, ctrl_id, sw, ip_src, ip_dst, port, port2);
        //     snprintf(buf, BUFSIZE, "%d%03d%s%s%03d%03d", ROUTE_ADD, sw, ip_src, ip_dst, port, port2);
        //     printf("buf:%s\n",buf);
        //     ret = send(cfd, buf, BUFSIZE, 0);
        //     if (ret == -1)
        //     {
        //         print_err("send route failed", __LINE__, errno);
        //         // 发送失败表示控制器断开连接，将 buf 存入对应的待执行结构 wait_exec_X
        //         Add_Wait_Exec(ctrl_id, buf, redis_ip);
        //     }
        // }
        sw_pre = sw;

        // 非源节点正常下发
        for(i = 1; i < reply->elements; i++)
        {
            sw = atoi(reply->element[i]->str)/1000;
            port = atoi(reply->element[i]->str)%1000;
            // printf("sw:%u, outport:%u\n", sw, port);
            ctrl_id = sw;
            db_id = Get_Ctrl_Conn_Db((uint32_t)ctrl_id, redis_ip);

            // 判断该出端口属于本区域交换机，向对应控制器发送通告
            // DB_ID = (((inet_addr(redis_ip))&0xff000000)>>24) - 1
            if(db_id == (((inet_addr(redis_ip))&0xff000000)>>24) - 1)
            {
                cfd = fd_ctl[ctrl_id]; 
                // printf("cfd:%ld\n", cfd);

                // 入端口
                port2 = sw_pre;
                // add route to routes set <-> link
                Add_Rt_Set((uint32_t)sw, (uint32_t)port, ip_src, ip_dst, 2, redis_ip);
                
                // type:1,sw:3,ip_src:8,ip_dst:8,outport:3,port2:3(port2字段用于承载入端口)
                memset(buf, 0, BUFSIZE);
                printf("\t\t非源节点下发, db_id:%d, ctrl_id:%d, sw:%d, ip_src:%s, ip_dst:%s, port:%d, port2:%d\n", db_id, ctrl_id, sw, ip_src, ip_dst, port, port2);
                snprintf(buf, BUFSIZE, "%d%03d%s%s%03d%03d", ROUTE_ADD, sw, ip_src, ip_dst, port, port2);
                printf("\t\tbuf:%s\n",buf);
                ret = send(cfd, buf, BUFSIZE, 0);
                if (ret == -1)
                {
                    print_err("\t\tsend route failed", __LINE__, errno);
                    // 发送失败表示控制器断开连接，将 buf 存入对应的待执行结构 wait_exec_X
                    Add_Wait_Exec(ctrl_id, buf, redis_ip);
                }
            }
            sw_pre = sw;
        }
    }

    free(arg);
    freeReplyObject(reply);
    redisFree(context);
    return NULL;
}

// 路由计算和调整
void *work_thread(void *redis_ip)
{
    // 校对topo将失效链路加入fail_link
    // Diff_Topo(slot, DB_ID, redis_ip);

/****************************************************************************/
    // 根据del_link遍历路由条目，进行修改
    char cmd[CMD_MAX_LENGHT] = {0};
    redisContext *context, *context1, *context2;
    redisReply *reply, *reply1, *reply2;
    uint64_t sw;
    uint32_t sw1, sw2;
    int i, j, k, a, b, c = 0;
    int ctrl_id = 0; // 记录控制器ID
    int db_id = 0;
    long cfd = -1;
    int ret = -1;
    char buf[BUFSIZE] = {0,};
    char ip_src[IP_LEN+1] = {0,};
    char ip_dst[IP_LEN+1] = {0,};
    char *ip_src_thread = NULL;
    char *ip_dst_thread = NULL;
    int num = 1; // 路由序号
    char ip_src_two[IP_LEN/4+1] = {0,}; // ip_src最后两位
    char ip_dst_two[IP_LEN/4+1] = {0,}; // ip_dst最后两位

    int matrix[MAX_NUM][MAX_NUM];
    memset(matrix, 0x3f, sizeof(matrix)); // 记录Floyd距离
    int matrix_ori[MAX_NUM][MAX_NUM];
    memset(matrix_ori, 0x3f, sizeof(matrix_ori)); // 记录原始拓扑
    int matrix_new[MAX_NUM][MAX_NUM];
    memset(matrix_new, 0x3f, sizeof(matrix_new)); // 记录用于Suurballe计算的拓扑
    uint32_t node1, node2 = 0;
    uint64_t delay = 0;
    int route[MAX_NUM][MAX_NUM]; // 记录松弛节点
    memset(route, -1, sizeof(route));
    int nextsw[MAX_NUM]; // 记录下一跳交换机节点
    int hop = 0;
    char out_sw_port[CMD_MAX_LENGHT] = {0,}; // 存储出端口列表
    char sw_port[8] = {0,}; // 存储出端口
    int fd_close[MAX_NUM] = {0,}; // 存储将要关闭的套接字
    int fd_close_num = 0; // 记录将要关闭的套接字的数量
    int flag_fd[MAX_NUM] = {0,}; // 标记是否被记录在fd_close中

    int mindist = 0;
    int minnode = -1;
    int curnode = -1;
    int prenode = -1;
    int flag = 0; // flag=1表示第一次dijkstra计算成功
    int node[MAX_NUM][4] = {0,}; // node[x][0]标记该节点是否已经加入最短路, node[x][1]表示到源节点到该节点的最短距离, node[x][2]记录该节点在最短路径上的前驱节点, node[x][4]记录该节点在最短路径上的后继节点
    int node_new[MAX_NUM][4] = {0,}; // 用于第二次dijkstra计算
    int path[MAX_NUM] = {0,}; // 第一次dijkstra的结果
    int path_new[MAX_NUM] = {0,}; // 第二次dijkstra的结果
    int path_1[MAX_NUM] = {0,}; // 链路分离路径1
    int path_2[MAX_NUM] = {0,}; // 链路分离路径2

    // int db_write_id = -1;
    // char db_write_ip[20] = "192.168.68.";  // 数据库IP

    // 读取拓扑
    printf("\tstart to read topo\n");
    snprintf(cmd, CMD_MAX_LENGHT, "hgetall real_topo");
    context = redisConnect(redis_ip, REDIS_SERVER_PORT);
    reply = (redisReply *)redisCommand(context, cmd);
    if (NULL == reply)
    {
        printf("\t%d execute command:%s failure\n", __LINE__, cmd);
        redisFree(context);
        return NULL;
    }
    for(i = 0; i < reply->elements; i++)
    {
        if(i % 2 ==0)// port
        {
            sw = atol(reply->element[i]->str);
            node1 = (uint32_t)((sw & 0xffffffff00000000) >> 32);
            node2 = (uint32_t)(sw & 0x00000000ffffffff);
        }
        else// delay
        {
            delay = atol(reply->element[i]->str);
            matrix_ori[node1][node2] = delay;
        }
    }
    for(i = 0; i < MAX_NUM; i++)
    {
        matrix_ori[i][i] = 0;
    }
    // for(a = 0; a < MAX_NUM; a++)
    // {
    //     for(b = 0; b < MAX_NUM; b++)
    //     {
    //         printf("matrix_ori[%02d][%02d]=%d, ", a, b, matrix_ori[a][b]);
    //     }
    //     printf("\n");
    // }
    freeReplyObject(reply);
    redisFree(context);
    printf("\tfinish to read topo\n");

    // 去掉下个时间片要删除的链路
    printf("\tstart to revise topo(del some links)\n");
    snprintf(cmd, CMD_MAX_LENGHT, "smembers del_link_%02d", slot);
    context = redisConnect(redis_ip, REDIS_SERVER_PORT);
    if (context->err)
    {
        printf("\tError: %s\n", context->errstr);
        redisFree(context);
        return NULL;
    }
    // printf("connect redis server success\n");

    reply = (redisReply *)redisCommand(context, cmd);
    if (reply != NULL && reply->elements != 0)
    {
        printf("\tdel_link num = %lu\n",reply->elements);
        for(i = 0; i < reply->elements; i++)
        {
            sw = atol(reply->element[i]->str);
            sw1 = (uint32_t)((sw & 0xffffffff00000000) >> 32);
            sw2 = (uint32_t)(sw & 0x00000000ffffffff);
            printf("\t\tdel_link: sw%02d<->sw%02d\n", sw1, sw2);
            matrix_ori[sw1][sw2] = MAX_DIST;
        }
    }
    freeReplyObject(reply);
    redisFree(context);

    // Floyd 计算任意两点间距离
    printf("\tstart to run Floyd\n");
    for(i = 0; i < MAX_NUM; i++)
    {
        for(j = 0; j < MAX_NUM; j++)
        {
            matrix[i][j] = matrix_ori[i][j];
        }
    }
    for(k = 0; k < MAX_NUM; k++)
    {//从0开始遍历每一个中间节点，代表允许经过的结点编号<=k 
        for(i = 0; i < MAX_NUM; i++)
        {
            for(j = 0; j < MAX_NUM; j++)
            {
                // if(matrix[i][k] == MAX_DIST || matrix[k][j] == MAX_DIST) 
                //     continue;//中间节点不可达 
                if(matrix[i][k] + matrix[k][j] < matrix[i][j])//经过中间节点，路径变短 
                {
                    // printf("matrix[%02d][%02d]=%d,matrix[%02d][%02d]=%d,matrix[%02d][%02d]=%d\n",i,k,matrix[i][k],k,j,matrix[k][j],i,j,matrix[i][j]);
                    matrix[i][j] = matrix[i][k] + matrix[k][j];
                    // printf("new matrix[%02d][%02d]=%d\n",i,j,matrix[i][j]);
                    if(matrix[i][j]<0) printf("matrix[%02d][%02d]小于零\n",i,j);
                    route[i][j] = k;
                }
            }
        }
    }
    printf("\tfinish to run Floyd\n");

    /*组装Redis命令*/
    snprintf(cmd, CMD_MAX_LENGHT, "smembers del_link_%02d", slot);

    /*连接redis*/
    context1 = redisConnect(redis_ip, REDIS_SERVER_PORT);
    if (context1->err)
    { 
        printf("\tError: %s\n", context1->errstr);
        redisFree(context1);
        return NULL;
    }
    // printf("connect redis server success\n");
    printf("\tslot_%d start to adjust del_link route\n", slot);

    /*执行redis命令*/
    reply1 = (redisReply *)redisCommand(context1, cmd);
    if (reply1 != NULL && reply1->elements != 0)
    {
        // 输出查询结果
        printf("\tdel_link num = %lu\n",reply1->elements);

        // 先处理普通路由（正在工作的c2d路由除外）
        for(i = 0; i < reply1->elements; i++)
        {
            sw = atol(reply1->element[i]->str);
            sw1 = (uint32_t)((sw & 0xffffffff00000000) >> 32);
            sw2 = (uint32_t)(sw & 0x00000000ffffffff);
            printf("\t\tdel_link: sw%02d<->sw%02d\n", sw1, sw2);

            // ctrl_id = sw1;
            // db_id = Get_Ctrl_Conn_Db((uint32_t)ctrl_id, redis_ip);

            // 查询相关的非定时路由
            /*组装Redis命令*/
            snprintf(cmd, CMD_MAX_LENGHT, "smembers rt_set_%02d_%02d", sw1, sw2);

            /*连接redis*/
            context2 = redisConnect(redis_ip, REDIS_SERVER_PORT);
            if (context2->err)
            { 
                printf("\t\tError: %s\n", context2->errstr);
                redisFree(context2);
                continue;
            }
            // printf("connect redis server success\n");

            /*执行redis命令*/
            reply2 = (redisReply *)redisCommand(context2, cmd);
            if (reply2 == NULL)
            {
                printf("\t\texecute command:%s failure\n", cmd);
                redisFree(context2);
                continue;
            }

            // 输出查询结果
            printf("\t\tdel route num = %lu\n",reply2->elements);
            if(reply2->elements == 0)
            {
                freeReplyObject(reply2);
                redisFree(context2);
                continue;
            }

            for(k = 0; k < reply2->elements; k++)
            {
                printf("\t\t\troute entry: %s\n",reply2->element[k]->str);
                strncpy(ip_src_two, reply2->element[k]->str+6, 2);
                sw = strtoi(ip_src_two, 2) - 1;
                ctrl_id = sw;
                db_id = Get_Ctrl_Conn_Db((uint32_t)ctrl_id, redis_ip);

                // 判断起点属于本区域交换机，删除旧的链路-路由映射，向数据库写入新路由
                // DB_ID = (((inet_addr(redis_ip))&0xff000000)>>24) - 1
                if(db_id == (((inet_addr(redis_ip))&0xff000000)>>24) - 1)
                {             
                    strncpy(ip_src, reply2->element[k]->str, IP_LEN);
                    strncpy(ip_dst, reply2->element[k]->str + IP_LEN, IP_LEN);
                    num = reply2->element[k]->str[IP_LEN*2] - '0';
                    // Del_Rt_Set(slot, ip_src, ip_dst, num, redis_ip);

                    strncpy(ip_dst_two, reply2->element[k]->str+IP_LEN+6, 2);
                    sw1 = strtoi(ip_src_two, 2) - 1;
                    sw2 = strtoi(ip_dst_two, 2) - 1;
                    
                    // 判断是d2d路由 && 源节点是本地数据库 -> 直接通告源节点出端口
                    if( (strstr(ip_dst, "44") != NULL) && (strstr(ip_src, "44") != NULL) )
                    {
                        // 把两条d2d路由的链路映射都删掉
                        // if(num == 1) Del_Rt_Set(slot, ip_src, ip_dst, 2, redis_ip);
                        // else Del_Rt_Set(slot, ip_src, ip_dst, 1, redis_ip);
                        Del_Rt_Set(slot, ip_src, ip_dst, 1, redis_ip);
                        Del_Rt_Set(slot, ip_src, ip_dst, 2, redis_ip);

                        // Suurballe 计算两点之间的路由
                        // 初始化
                        printf("\t\t\tstart to run Suurballe\n");
                        strncpy(ip_dst_two, reply2->element[k]->str+IP_LEN+6, 2);
                        sw1 = strtoi(ip_src_two, 2) - 1;
                        sw2 = strtoi(ip_dst_two, 2) - 1;
                        mindist = 0;
                        minnode = -1;
                        curnode = -1;
                        prenode = -1;
                        flag = 0; 
                        memset(path, -1, sizeof(path));
                        memset(path_new, -1, sizeof(path_new));
                        memset(path_1, -1, sizeof(path_1));
                        memset(path_2, -1, sizeof(path_2));
                        for(a = 0; a < MAX_NUM; a++)
                        {
                            node[a][0] = 0;
                            node[a][1] = MAX_DIST;
                            node[a][2] = -1;
                            node[a][3] = -1;
                            node_new[a][0] = 0;
                            node_new[a][1] = MAX_DIST;
                            node_new[a][2] = -1;
                            node_new[a][3] = -1;
                        }
                        for(a = 0; a < MAX_NUM; a++)
                        {
                            for(b = 0; b < MAX_NUM; b++)
                            {
                                matrix_new[a][b] = matrix_ori[a][b];
                            }
                        }

                        // Dijkstra 计算两点间距离
                        node[sw1][0] = 1;
                        node[sw1][1] = 0;
                        for(a = 0; a < MAX_NUM; a++)
                        {
                            if(node[a][0] == 0 && matrix_new[sw1][a] < MAX_DIST)
                            {
                                node[a][1] = matrix_new[sw1][a];
                                node[a][2] = sw1;
                            }
                        }
                        for(a = 0; a < MAX_NUM; a++)
                        {
                            mindist = MAX_DIST;
                            minnode = -1;
                            for(b = 0; b < MAX_NUM; b++)
                            {
                                if(node[b][0] != 0) continue;
                                if(node[b][1] < mindist)
                                {
                                    mindist = node[b][1];
                                    minnode = b;
                                }
                            }

                            if(minnode == -1) break;
                            if(minnode == sw2) flag = 1;

                            node[minnode][0] = 1;
                            for(b = 0; b < MAX_NUM; b++)
                            {
                                if(node[b][0] == 0 && node[b][1] > matrix_new[minnode][b] + node[minnode][1] && matrix_new[minnode][b] < MAX_DIST)
                                {
                                    node[b][1] = matrix_new[minnode][b] + node[minnode][1];
                                    node[b][2] = minnode;
                                }
                            }
                        }

                        // 生成残差图
                        if(flag == 1)
                        {
                            for(a = 0; a < MAX_NUM; a++)
                            {
                                for(b = 0; b < MAX_NUM; b++)
                                {
                                    if(matrix_new[a][b]!=MAX_DIST)
                                        matrix_new[a][b] = matrix_new[a][b] + node[a][1] - node[b][1];
                                }
                            }
                            curnode = sw2;
                            prenode = node[curnode][2];
                            c = 0;
                            path[c++] = curnode;
                            while(prenode != -1)
                            {
                                node[prenode][3] = curnode;
                                matrix_new[curnode][prenode] = 0;
                                matrix_new[prenode][curnode] = MAX_DIST;
                                curnode = prenode;
                                prenode = node[curnode][2];
                                path[c++] = curnode;
                            }
                            
                            printf("db%d - db%d: \n", sw1, sw2);
                            c = 0;
                            printf("\tpath: ");
                            while(path[c] != -1)
                            {
                                printf("%d ", path[c++]);
                            }
                            printf("\n");

                            // 第二次Dijkstra计算
                            node_new[sw1][0] = 1;
                            node_new[sw1][1] = 0;
                            for(a = 0; a < MAX_NUM; a++)
                            {
                                if(node_new[a][0] == 0 && matrix_new[sw1][a] != MAX_DIST)
                                {
                                    node_new[a][1] = matrix_new[sw1][a];
                                    node_new[a][2] = sw1;
                                }
                            }
                            for(a = 0; a < MAX_NUM; a++)
                            {
                                mindist = MAX_DIST;
                                minnode = -1;
                                for(b = 0; b < MAX_NUM; b++)
                                {
                                    if(node_new[b][0] != 0) continue;
                                    if(node_new[b][1] < mindist)
                                    {
                                        mindist = node_new[b][1];
                                        minnode = b;
                                    }
                                }

                                if(minnode == -1) break;
                                if(minnode == sw2) break;

                                node_new[minnode][0] = 1;
                                for(b = 0; b < MAX_NUM; b++)
                                {
                                    if(node_new[b][0] == 0 && node_new[b][1] > matrix_new[minnode][b] + node_new[minnode][1] && matrix_new[minnode][b] != MAX_DIST)
                                    {
                                        node_new[b][1] = matrix_new[minnode][b] + node_new[minnode][1];
                                        node_new[b][2] = minnode;
                                    }
                                }
                            }
                            if(minnode == sw2)
                            {
                                curnode = sw2;
                                prenode = node_new[curnode][2];
                                c = 0;
                                path_new[c++] = curnode;
                                while(prenode != -1)
                                {
                                    node_new[prenode][3] = curnode;
                                    curnode = prenode;
                                    prenode = node_new[curnode][2];
                                    path_new[c++] = curnode;
                                }

                                c = 0;
                                printf("\tpath_new: ");
                                while(path_new[c] != -1)
                                {
                                    printf("%d ", path_new[c++]);
                                }
                                printf("\n");

                                // 比较两条路径，删除重复部分
                                a = 0;
                                while(path[a+1] != sw1)
                                {
                                    b = 0;
                                    while(path_new[b+1] != sw1)
                                    {
                                        if(path_new[b] == path[a+1] && path_new[b+1] == path[a])
                                        {
                                            node[path[a+1]][3] = -1;
                                            node_new[path[a]][3] = -1;
                                            // printf("del overlap link sw%d - sw%d\n", path[i+1], path[i]);
                                        }
                                        b++;
                                    }
                                    a++;
                                }

                                // 重组路径
                                path_1[0] = sw1;
                                path_1[1] = node[sw1][3];
                                curnode = path_1[1];
                                c = 2;
                                while(curnode != sw2)
                                {
                                    if(node[curnode][3] == -1)
                                    {
                                        path_1[c] = node_new[curnode][3];
                                        node_new[curnode][3] = -1;
                                    }  
                                    else
                                    {
                                        path_1[c] = node[curnode][3];
                                        node[curnode][3] = -1;
                                    }
                                    curnode = path_1[c];
                                    c++;
                                }
                                path_2[0] = sw1;
                                path_2[1] = node_new[sw1][3];
                                curnode = path_2[1];
                                c = 2;
                                while(curnode != sw2)
                                {
                                    if(node[curnode][3] == -1)
                                        path_2[c] = node_new[curnode][3];
                                    else
                                        path_2[c] = node[curnode][3];
                                    curnode = path_2[c];
                                    c++;
                                }
                                
                                printf("db%d - db%d: \n", sw1, sw2);
                                c = 0;
                                printf("\tdel_link d2d new path_1: ");
                                while(path_1[c] != -1)
                                {
                                    printf("%d ", path_1[c++]);
                                }
                                printf("\n");
                                c = 0;
                                printf("\tdel_link d2d new path_2: ");
                                while(path_2[c] != -1)
                                {
                                    printf("%d ", path_2[c++]);
                                }
                                printf("\n\n");

                                // 向数据库写入2条新路由
                                // db_write_id = db_write_select(sw2);
                                // memset(&db_write_ip[11], 0, 9);
                                // sprintf(&db_write_ip[11], "%d", db_write_id+1);

                                c = 0;
                                while(path_1[c+1] != -1)
                                {
                                    snprintf(sw_port, 8, "%03d%03d ", path_1[c], path_1[c+1]);
                                    strncpy(out_sw_port + c * 7, sw_port, 7);
                                    c++;
                                }
                                Set_Cal_Route(ip_src, ip_dst, 1, out_sw_port, redis_ip);
                                memset(out_sw_port, 0, CMD_MAX_LENGHT);
                                // 间隔20ms
                                usleep(20000);
                                c = 0;
                                while(path_2[c+1] != -1)
                                {
                                    snprintf(sw_port, 8, "%03d%03d ", path_2[c], path_2[c+1]);
                                    strncpy(out_sw_port + c * 7, sw_port, 7);
                                    c++;
                                }
                                Set_Cal_Route(ip_src, ip_dst, 2, out_sw_port, redis_ip);
                                memset(out_sw_port, 0, CMD_MAX_LENGHT);
 
                                // add route to routes set <-> link
                                Add_Rt_Set((uint32_t)path_1[0], (uint32_t)path_1[1], ip_src, ip_dst, 1, redis_ip);
                                Add_Rt_Set((uint32_t)path_1[0], (uint32_t)path_2[1], ip_src, ip_dst, 2, redis_ip);
                                // 通告源节点出端口
                                ip_src_thread = malloc(sizeof(char)*(IP_LEN+1));
                                memset(ip_src_thread, 0, sizeof(char));
                                ip_dst_thread = malloc(sizeof(char)*(IP_LEN+1));
                                memset(ip_dst_thread, 0, sizeof(char));
                                strncpy(ip_src_thread, ip_src, IP_LEN);
                                strncpy(ip_dst_thread, ip_dst, IP_LEN);
                                route_notice(db_id, path_1[0], ip_src_thread, ip_dst_thread, path_1[1], path_2[1], redis_ip);
                            }
                            else
                            {
                                printf("\t\t\tdel_link d2d new path_2 failed\n");
                                // 向数据库写入1条新路由
                                // db_write_id = db_write_select(sw2);
                                // memset(&db_write_ip[11], 0, 9);
                                // sprintf(&db_write_ip[11], "%d", db_write_id+1);

                                c = 0;
                                while(path[c+1] != -1) c++;
                                a = 0;
                                while(c > 0)
                                {
                                    snprintf(sw_port, 8, "%03d%03d ", path[c], path[c-1]);
                                    strncpy(out_sw_port + a * 7, sw_port, 7);
                                    c--;
                                    a++;
                                }
                                Set_Cal_Route(ip_src, ip_dst, 1, out_sw_port, redis_ip);
                                memset(out_sw_port, 0, CMD_MAX_LENGHT);

                                c = 0;
                                while(path[c+1] != -1) c++;
                                // add route to routes set <-> link
                                Add_Rt_Set((uint32_t)path[c], (uint32_t)path[c-1], ip_src, ip_dst, 1, redis_ip);
                                // 通告源节点出端口
                                ip_src_thread = malloc(sizeof(char)*(IP_LEN+1));
                                memset(ip_src_thread, 0, sizeof(char));
                                ip_dst_thread = malloc(sizeof(char)*(IP_LEN+1));
                                memset(ip_dst_thread, 0, sizeof(char));
                                strncpy(ip_src_thread, ip_src, IP_LEN);
                                strncpy(ip_dst_thread, ip_src, IP_LEN);
                                route_notice(db_id, path[c], ip_src_thread, ip_dst_thread, path[c-1], 999, redis_ip);
                            }
                        }
                        else
                        {
                            printf("\t\t\tdel_link d2d new paths failed\n");
                        }
                        printf("\t\t\tfinish to run Suurballe\n");  
                    }
                    // 判断是 正在工作的 c2d控制通道路由
                    // 确定源是本区域的控制器，需要判断目的是本地数据库
                    else if( (strstr(ip_src, "44") == NULL) && (strstr(ip_dst, "44") != NULL) && (sw2 == db_id) )
                    {
                        continue;
                    }
                    // 判断是 正在工作的 d2c控制通道路由
                    // 确定源是本地数据库，需要判断目的是本区域控制器
                    else if( (strstr(ip_src, "44") != NULL) && (strstr(ip_dst, "44") == NULL) && (Get_Ctrl_Conn_Db((uint32_t)sw2, redis_ip) == db_id) )
                    {
                        continue;
                    }
                    else
                    {
                        Del_Rt_Set(slot, ip_src, ip_dst, num, redis_ip);
                        // 向数据库写入新路由
                        printf("\t\t\tdel_link sw%02d<->sw%02d new route write to Redis\n", sw1, sw2);
                        printf("\t\t\tsw%02d<->sw%02d dist = %d\n", sw1, sw2, matrix[sw1][sw2]);
                        if(matrix[sw1][sw2] != MAX_DIST)
                        {
                            printf("\t\t\tstart to generate route_sw%02d_sw%02d\n", sw1, sw2);
                            // for(a = 0; a < MAX_NUM; a++)
                            // {
                            //     for(b = 0; b < MAX_NUM; b++)
                            //     {
                            //         printf("route[%02d][%02d]=%d, ", a, b, route[a][b]);
                            //     }
                            //     printf("\n");
                            // }
                            hop = 0;
                            nextsw[hop++] = sw1;
                            out(sw1, sw2, &route[0][0], nextsw, &hop);
                            nextsw[hop] = sw2;
                            
                            printf("\t\t\troute_sw%02d_sw%02d: ", sw1, sw2);
                            for(j = 0; j < hop; j++)
                            {
                                snprintf(sw_port, 8, "%03d%03d ", nextsw[j], nextsw[j+1]);
                                strncpy(out_sw_port + j * 7, sw_port, 7);
                                printf("%03d ", nextsw[j]);
                            }
                            printf("%03d\n", nextsw[j]);
                            Set_Cal_Route(ip_src, ip_dst, 1, out_sw_port, redis_ip);
                            memset(out_sw_port, 0, CMD_MAX_LENGHT);
                        }
                    } 
                }
            }
            
            freeReplyObject(reply2);
            redisFree(context2);
        }

        // 等待其他路由调整完成之后，通知相应控制器切换数据库，最后调整c2d工作路由
        sleep(5);
        for(i = 0; i < reply1->elements; i++)
        {
            sw = atol(reply1->element[i]->str);
            sw1 = (uint32_t)((sw & 0xffffffff00000000) >> 32);
            sw2 = (uint32_t)(sw & 0x00000000ffffffff);
            printf("\t\tdel_link: sw%02d<->sw%02d\n", sw1, sw2);

            // ctrl_id = sw1;
            // db_id = Get_Ctrl_Conn_Db((uint32_t)ctrl_id, redis_ip);

            // 查询相关的非定时路由
            /*组装Redis命令*/
            snprintf(cmd, CMD_MAX_LENGHT, "smembers rt_set_%02d_%02d", sw1, sw2);

            /*连接redis*/
            context2 = redisConnect(redis_ip, REDIS_SERVER_PORT);
            if (context2->err)
            { 
                printf("\t\tError: %s\n", context2->errstr);
                redisFree(context2);
                continue;
            }
            // printf("connect redis server success\n");

            /*执行redis命令*/
            reply2 = (redisReply *)redisCommand(context2, cmd);
            if (reply2 == NULL)
            {
                printf("\t\texecute command:%s failure\n", cmd);
                redisFree(context2);
                continue;
            }

            // 输出查询结果
            printf("\t\tdel_link_sw%02d_sw%02d route num = %lu\n", sw1, sw2, reply2->elements);
            if(reply2->elements == 0)
            {
                freeReplyObject(reply2);
                redisFree(context2);
                continue;
            }

            for(k = 0; k < reply2->elements; k++)
            {
                printf("\t\t\troute entry: %s\n",reply2->element[k]->str);
                strncpy(ip_src_two, reply2->element[k]->str+6, 2);
                sw = strtoi(ip_src_two, 2) - 1;
                ctrl_id = sw;
                db_id = Get_Ctrl_Conn_Db((uint32_t)ctrl_id, redis_ip);

                // 判断起点属于本区域交换机，删除旧的链路-路由映射，向数据库写入新路由
                // DB_ID = (((inet_addr(redis_ip))&0xff000000)>>24) - 1
                if(db_id == (((inet_addr(redis_ip))&0xff000000)>>24) - 1)
                {             
                    strncpy(ip_src, reply2->element[k]->str, IP_LEN);
                    strncpy(ip_dst, reply2->element[k]->str + IP_LEN, IP_LEN);
                    num = reply2->element[k]->str[IP_LEN*2] - '0';
                    Del_Rt_Set(slot, ip_src, ip_dst, num, redis_ip);

                    strncpy(ip_dst_two, reply2->element[k]->str+IP_LEN+6, 2);
                    sw1 = strtoi(ip_src_two, 2) - 1;
                    sw2 = strtoi(ip_dst_two, 2) - 1;
                    
                    // 判断是 正在工作的 c2d控制通道路由
                    // 确定源是本区域的控制器，需要判断目的是本地数据库
                    if( (strstr(ip_src, "44") == NULL) && (strstr(ip_dst, "44") != NULL) && (sw2 == db_id) )
                    {
                        // 优雅关闭tcp套接字，通知控制器切换数据库
                        if(flag_fd[sw1] == 0)
                        {
                            printf("\t\t\twait to shutdown ctrl_%d socket_%d\n", sw1, fd_ctl[sw1]);
                            // shutdown(fd[sw1], SHUT_WR);
                            fd_close[fd_close_num++] = fd_ctl[sw1];
                            flag_fd[sw1] = 1;
                        }
                    }
                    // 判断是 正在工作的 d2c控制通道路由
                    // 确定源是本地数据库，需要判断目的是本区域控制器
                    else if( (strstr(ip_src, "44") != NULL) && (strstr(ip_dst, "44") == NULL) && (Get_Ctrl_Conn_Db((uint32_t)sw2, redis_ip) == db_id) )
                    {
                        // 优雅关闭tcp套接字，通知控制器切换数据库
                        if(flag_fd[sw2] == 0)
                        {
                            printf("\t\t\twait to shutdown ctrl_%d socket_%d\n", sw2, fd_ctl[sw2]);
                            // shutdown(fd[sw2], SHUT_WR);
                            fd_close[fd_close_num++] = fd_ctl[sw2];
                            flag_fd[sw2] = 1;
                        }
                    }
                }
            }
            
            // 通知相应控制器切换数据库，然后调整c2d工作路由
            for(j = 0; j < fd_close_num; j++)
            {
                printf("\t\tshutdown socket %d\n", fd_close[j]);
                shutdown(fd_close[j], SHUT_WR);
                // close(fd_close[j]);
            }
            memset(fd_close, 0, sizeof(fd_close));
            fd_close_num = 0;
            for(j = 0; j < MAX_NUM; j++)
            {
                if(flag_fd[j] == 1)
                {
                    fd_ctl[j] = 0;
                }
            }
            memset(flag_fd, 0, sizeof(flag_fd));
            
            for(k = 0; k < reply2->elements; k++)
            {
                printf("\t\t\troute entry: %s\n",reply2->element[k]->str);
                strncpy(ip_src_two, reply2->element[k]->str+6, 2);
                sw = strtoi(ip_src_two, 2) - 1;
                ctrl_id = sw;
                db_id = Get_Ctrl_Conn_Db((uint32_t)ctrl_id, redis_ip);

                // 判断起点属于本区域交换机，删除旧的链路-路由映射，向数据库写入新路由
                // DB_ID = (((inet_addr(redis_ip))&0xff000000)>>24) - 1
                if(db_id == (((inet_addr(redis_ip))&0xff000000)>>24) - 1)
                {             
                    strncpy(ip_src, reply2->element[k]->str, IP_LEN);
                    strncpy(ip_dst, reply2->element[k]->str + IP_LEN, IP_LEN);
                    num = reply2->element[k]->str[IP_LEN*2] - '0';
                    // Del_Rt_Set(slot, ip_src, ip_dst, num, redis_ip);
                    
                    // 判断是 正在工作的 c2d控制通道路由
                    // 确定源是本区域的控制器，需要判断目的是本地数据库
                    if( (strstr(ip_src, "44") == NULL) && (strstr(ip_dst, "44") != NULL) && (sw2 == db_id) )
                    {
                        // 向数据库写入新路由
                        if(matrix[sw1][sw2] != MAX_DIST)
                        {
                            hop = 0;
                            nextsw[hop++] = sw1;
                            out(sw1, sw2, &route[0][0], nextsw, &hop);
                            nextsw[hop] = sw2;

                            for(j = 0; j < hop; j++)
                            {
                                snprintf(sw_port, 8, "%03d%03d ", nextsw[j], nextsw[j+1]);
                                strncpy(out_sw_port + j * 7, sw_port, 7);
                            }
                            Set_Cal_Route(ip_src, ip_dst, 1, out_sw_port, redis_ip);
                            memset(out_sw_port, 0, CMD_MAX_LENGHT);
                        }
                    }
                    // 判断是 正在工作的 d2c控制通道路由
                    // 确定源是本地数据库，需要判断目的是本区域控制器
                    if( (strstr(ip_src, "44") != NULL) && (strstr(ip_dst, "44") == NULL) && (Get_Ctrl_Conn_Db((uint32_t)sw2, redis_ip) == db_id) )
                    {
                        // 向数据库写入新路由
                        if(matrix[sw1][sw2] != MAX_DIST)
                        {
                            hop = 0;
                            nextsw[hop++] = sw1;
                            out(sw1, sw2, &route[0][0], nextsw, &hop);
                            nextsw[hop] = sw2;

                            for(j = 0; j < hop; j++)
                            {
                                snprintf(sw_port, 8, "%03d%03d ", nextsw[j], nextsw[j+1]);
                                strncpy(out_sw_port + j * 7, sw_port, 7);
                            }
                            Set_Cal_Route(ip_src, ip_dst, 1, out_sw_port, redis_ip);
                            memset(out_sw_port, 0, CMD_MAX_LENGHT);
                        }
                    }
                }
            }

            freeReplyObject(reply2);
            redisFree(context2);
        }
    }
    
    printf("\tslot_%d del_link上的全部路由调整已完成\n", slot);
    freeReplyObject(reply1);
    redisFree(context1);
    return NULL;
}

// udp 子线程，接收时间片序号
void *udpconnect(void *redis_ip)
{
    uint8_t buf[BUFSIZE] = {'\0'};
    struct sockaddr_in *clent_addr;
    socklen_t len = sizeof(struct sockaddr_in);
    pthread_t pid;
    long ret = -1;
    char ip_two[IP_LEN/4+1] = {0,}; // redis_ip最后两位
    strncpy(ip_two, redis_ip+11, 2);
    int redis_id = atoi(ip_two)-1;
    int i = 0;
    
    if(listen_init(redis_ip) != 0)
    {
        printf("套接字初始化失败\n"); 
        return NULL;
    }
    while(1)
    {
        bzero(&buf, sizeof(buf));
        recvfrom(server_fd_udp, buf, BUFSIZE, 0, (struct sockaddr*)clent_addr, &len);
        slot = atoi(buf);
        // memset(fail_link_index, 0 , sizeof(fail_link_index));
        // 将下一个时间片对应的index归零
        printf("slot_%d fail_link index memset to 0\n", (slot+1)%SLOT_NUM);
        for(i = 0; i < MAX_NUM; i++)
        {
            fail_link_index[(slot+1+SLOT_NUM)%SLOT_NUM][i] = 0;
        }

        // wait converge
        // sleep(SLOT_TIME/2);
        //创建子线程，根据del_link遍历路由条目进行修改
        ret = pthread_create(&pid, NULL, work_thread, redis_ip);
        if (ret == -1) 
        {
            print_err("create work_thread failed", __LINE__, errno); 
        }
        printf("create work_thread success\n");
    }
}

// 解析传入的路由条目，向相应的控制器发送新增流表项通告
int route_add(char *obj, char *redis_ip)
{
    char cmd[CMD_MAX_LENGHT] = {0};
    redisContext *context;
    redisReply *reply;
    int i = 0;
    int ctrl_id = 0; // 记录控制器ID
    int db_id = 0;
    int sw = 0;
    int port = 0;
    long cfd = -1;
    int ret = -1;
    char buf[BUFSIZE] = {0};
    int num = 0;
    pthread_t pid;

    char ip_src[IP_LEN+1] = {0,};
    char ip_dst[IP_LEN+1] = {0,};
    int port2 = 999;
    strncpy(ip_src, &obj[6], IP_LEN);
    strncpy(ip_dst, &obj[6 + IP_LEN], IP_LEN);
    num = obj[IP_LEN*2+7] - '0';

    // 判断是d2d路由
    if(((strstr(ip_dst, "44") != NULL) && (strstr(ip_src, "44") != NULL)))
    {
        struct para *d2d_para = malloc(sizeof(struct para));
        d2d_para->obj = obj;
        d2d_para->redis_ip = redis_ip;
        //创建子线程，汇总两条d2d路由信息后下发
        ret = pthread_create(&pid, NULL, d2d_thread, d2d_para);
        if (ret == -1) 
        {
            print_err("create d2d_thread failed", __LINE__, errno); 
        }
        printf("create d2d_thread success\n");
        return 0;
    }

    /*组装Redis命令*/
    snprintf(cmd, CMD_MAX_LENGHT, "lrange %s 0 -1", obj);

    /*连接redis*/
    context = redisConnect(redis_ip, REDIS_SERVER_PORT);
    if (context->err)
    { 
        printf("Error: %s\n", context->errstr);
        redisFree(context);
        return -1;
    }
    // printf("connect redis server success\n");

    /*执行redis命令*/
    reply = (redisReply *)redisCommand(context, cmd);
    if (reply == NULL)
    {
        printf("execute command:%s failure\n", cmd);
        redisFree(context);
        return -1;
    }

    // 输出查询结果
    // printf("entry num = %lu\n",reply->elements);
    if(reply->elements == 0) 
    {
        freeReplyObject(reply);
        redisFree(context);
        return -1;
    }
    for(i = 0; i < reply->elements; i++)
    {
        // printf("out_sw_port: %s\n",reply->element[i]->str);
        sw = atoi(reply->element[i]->str)/1000;
        port = atoi(reply->element[i]->str)%1000;
        // printf("sw:%u, outport:%u\n", sw, port);
        ctrl_id = sw;
        db_id = Get_Ctrl_Conn_Db((uint32_t)ctrl_id, redis_ip);

        // 判断该出端口属于本区域交换机，向对应控制器发送通告
        // DB_ID = (((inet_addr(redis_ip))&0xff000000)>>24) - 1
        if(db_id == (((inet_addr(redis_ip))&0xff000000)>>24) - 1)
        {
            cfd = fd_ctl[ctrl_id]; 
            // printf("cfd:%ld\n", cfd);

            // 初始化999表示没有第二个出端口
            port2 = 999;
            // add route to routes set <-> link
            Add_Rt_Set((uint32_t)sw, (uint32_t)port, ip_src, ip_dst, num, redis_ip);

            // type:1,sw:3,ip_src:8,ip_dst:8,outport:3,port2:3
            memset(buf, 0, BUFSIZE);
            printf("\tdb_id:%d, ctrl_id:%d, sw:%d, ip_src:%s, ip_dst:%s, port:%d, port2:%d\n", db_id, ctrl_id, sw, ip_src, ip_dst, port, port2);
            snprintf(buf, BUFSIZE, "%d%03d%s%s%03d%03d", ROUTE_ADD, sw, ip_src, ip_dst, port, port2);
            printf("\tbuf:%s\n",buf);
            ret = send(cfd, buf, BUFSIZE, 0);
            if (ret == -1)
            {
                print_err("\tsend route failed", __LINE__, errno);
                // 发送失败表示控制器断开连接，将 buf 存入对应的待执行结构 wait_exec_X
                Add_Wait_Exec(ctrl_id, buf, redis_ip);
            }
        }
    }

    freeReplyObject(reply);
    redisFree(context);
    free(obj);
    return 0;
}

// 遍历传入的失效链路，将失效链路上的全部路由都调整为可行的新路由，向相应的控制器发送 删除/新增 流表项通告
// 计算新路由时采用的真实拓扑real_topo，可能会在新路由下发过程中发生变化，为了保证路由调整的有效性，控制器在下发新增流表前需要判断其合法性
int route_del(char *obj, int index, char *redis_ip)
{
    char cmd[CMD_MAX_LENGHT] = {0};
    redisContext *context;
    redisReply *reply;
    int i, j, k, a, b, c = 0;
    int ctrl_id = 0; // 记录控制器ID
    int db_id = 0;
    uint32_t sw1, sw2 = 0;
    uint32_t fail_sw1, fail_sw2 = 0;
    uint64_t sw = 0;
    int port = 0;
    long cfd = -1;
    int ret = -1;
    char buf[BUFSIZE] = {0,};
    char ip_src[IP_LEN+1] = {0,};
    char ip_dst[IP_LEN+1] = {0,};
    char *ip_src_thread = NULL;
    char *ip_dst_thread = NULL;
    int num = 1; // 路由序号
    char ip_src_two[IP_LEN/4+1] = {0,}; // ip_src最后两位
    char ip_dst_two[IP_LEN/4+1] = {0,}; // ip_dst最后两位

    // char slot_str[2] = {0,};
    // strncpy(slot_str, obj+10, 2);
    // int slot = atoi(slot_str);

    int matrix[MAX_NUM][MAX_NUM];
    memset(matrix, 0x3f, sizeof(matrix)); // 记录Floyd距离
    int matrix_ori[MAX_NUM][MAX_NUM];
    memset(matrix_ori, 0x3f, sizeof(matrix_ori)); // 记录原始拓扑
    int matrix_new[MAX_NUM][MAX_NUM];
    memset(matrix_new, 0x3f, sizeof(matrix_new)); // 记录用于Suurballe计算的拓扑
    uint32_t node1, node2 = 0;
    uint64_t delay = 0;
    int route[MAX_NUM][MAX_NUM]; // 记录松弛节点
    memset(route, -1, sizeof(route));
    int nextsw[MAX_NUM]; // 记录下一跳交换机节点
    int hop = 0;
    char out_sw_port[CMD_MAX_LENGHT] = {0,}; // 存储出端口列表
    char sw_port[8] = {0,}; // 存储出端口

    int mindist = 0;
    int minnode = -1;
    int curnode = -1;
    int prenode = -1;
    int flag = 0; // flag=1表示第一次dijkstra计算成功
    int node[MAX_NUM][4] = {0,}; // node[x][0]标记该节点是否已经加入最短路, node[x][1]表示到源节点到该节点的最短距离, node[x][2]记录该节点在最短路径上的前驱节点, node[x][4]记录该节点在最短路径上的后继节点
    int node_new[MAX_NUM][4] = {0,}; // 用于第二次dijkstra计算
    int path[MAX_NUM] = {0,}; // 第一次dijkstra的结果
    int path_new[MAX_NUM] = {0,}; // 第二次dijkstra的结果
    int path_1[MAX_NUM] = {0,}; // 链路分离路径1
    int path_2[MAX_NUM] = {0,}; // 链路分离路径2

    int db_write_id = -1;
    char db_write_ip[20] = "192.168.68.";  // 数据库IP

    /*组装Redis命令*/
    snprintf(cmd, CMD_MAX_LENGHT, "lindex %s %d", obj, index);

    /*连接redis*/
    context = redisConnect(redis_ip, REDIS_SERVER_PORT);
    if (context->err)
    {
        printf("Error: %s\n", context->errstr);
        redisFree(context);
        return -1;
    }
    // printf("connect redis server success\n");

    /*执行redis命令*/
    reply = (redisReply *)redisCommand(context, cmd);
    if (reply == NULL)
    {
        printf("execute command:%s failure\n", cmd);
        redisFree(context);
        return -1;
    }

    // 输出查询结果
    if(reply->str == NULL)
    {
        printf("return NULL\n");
        freeReplyObject(reply);
        redisFree(context);
        return -1;
    }
    sw = atol(reply->str);
    fail_sw1 = (uint32_t)((sw & 0xffffffff00000000) >> 32);
    fail_sw2 = (uint32_t)(sw & 0x00000000ffffffff); // 失效节点
    printf("\t%s_index%d: sw%d<->sw%d\n", obj, index, fail_sw1, fail_sw2); 
    freeReplyObject(reply);
    redisFree(context);

    // 读取拓扑
    printf("\tstart to read topo\n");
    snprintf(cmd, CMD_MAX_LENGHT, "hgetall real_topo");
    context = redisConnect(redis_ip, REDIS_SERVER_PORT);
    reply = (redisReply *)redisCommand(context, cmd);
    if (NULL == reply)
    {
        printf("\t%d execute command:%s failure\n", __LINE__, cmd);
        redisFree(context);
        return FAILURE;
    }
    for(i = 0; i < reply->elements; i++)
    {
        if(i % 2 ==0)// port
        {
            sw = atol(reply->element[i]->str);
            node1 = (uint32_t)((sw & 0xffffffff00000000) >> 32);
            node2 = (uint32_t)(sw & 0x00000000ffffffff);
        }
        else// delay
        {
            delay = atol(reply->element[i]->str);
            if(node1 != fail_sw2 && node2 != fail_sw2)
                matrix_ori[node1][node2] = delay;
            else // 在计算新路由时， 将失效链路的対端节点相关联的链路时延设置为一个比较大的常数，使得新路由尽量避开对端节点，以防止节点失效导致的路由多次调整
                matrix_ori[node1][node2] = 1e7;
        }
    }
    matrix_ori[fail_sw1][fail_sw2] = MAX_DIST;
    matrix_ori[fail_sw2][fail_sw1] = MAX_DIST;
    for(i = 0; i < MAX_NUM; i++)
    {
        matrix_ori[i][i] = 0;
    }
    freeReplyObject(reply);
    redisFree(context);

    // 去掉下个时间片要删除的链路
    printf("\tstart to revise topo(del some links)\n");
    snprintf(cmd, CMD_MAX_LENGHT, "smembers del_link_%02d", slot);
    context = redisConnect(redis_ip, REDIS_SERVER_PORT);
    if (context->err)
    {
        printf("Error: %s\n", context->errstr);
        redisFree(context);
        return FAILURE;
    }
    // printf("connect redis server success\n");

    reply = (redisReply *)redisCommand(context, cmd);
    if (reply != NULL && reply->elements != 0)
    {
        printf("\tdel_link num = %lu\n",reply->elements);

        for(i = 0; i < reply->elements; i++)
        {
            sw = atol(reply->element[i]->str);
            sw1 = (uint32_t)((sw & 0xffffffff00000000) >> 32);
            sw2 = (uint32_t)(sw & 0x00000000ffffffff);
            printf("\t\tdel_link: sw%02d<->sw%02d\n", sw1, sw2);
            matrix_ori[sw1][sw2] = MAX_DIST;
        }
    }
    freeReplyObject(reply);
    redisFree(context);

    // Floyd 计算任意两点间距离
    printf("\tstart to run Floyd\n");
    for(i = 0; i < MAX_NUM; i++)
    {
        for(j = 0; j < MAX_NUM; j++)
        {
            matrix[i][j] = matrix_ori[i][j];
        }
    }
    for(k = 0; k < MAX_NUM; k++)
    {//从0开始遍历每一个中间节点，代表允许经过的结点编号<=k 
        for(i = 0; i < MAX_NUM; i++)
        {
            for(j = 0; j < MAX_NUM; j++)
            {
                // if(matrix[i][k] == MAX_DIST || matrix[k][j] == MAX_DIST) 
                //     continue;//中间节点不可达 
                if(matrix[i][k] + matrix[k][j] < matrix[i][j])//经过中间节点，路径变短 
                {
                    matrix[i][j] = matrix[i][k] + matrix[k][j];
                    route[i][j] = k;
                }
            }
        }
    }
    printf("\tfinish to run Floyd\n");

    /*组装Redis命令*/
    snprintf(cmd, CMD_MAX_LENGHT, "smembers rt_set_%02d_%02d", fail_sw1, fail_sw2);

    /*连接redis*/
    context = redisConnect(redis_ip, REDIS_SERVER_PORT);
    if (context->err)
    {
        printf("Error: %s\n", context->errstr);
        redisFree(context);
        return -1;
    }
    // printf("connect redis server success\n");

    /*执行redis命令*/
    reply = (redisReply *)redisCommand(context, cmd);
    if (reply == NULL)
    {
        printf("execute command:%s failure\n", cmd);
        redisFree(context);
        return -1;
    }

    // 输出查询结果
    printf("\tfail_link_sw%02d_sw%02d route num = %lu\n", fail_sw1, fail_sw2, reply->elements);
    if(reply->elements == 0) 
    {
        freeReplyObject(reply);
        redisFree(context);
        return -1;
    }
    for(i = 0; i < reply->elements; i++)
    {
        printf("\t\tfail route entry: %s\n",reply->element[i]->str);
        strncpy(ip_src_two, reply->element[i]->str+6, 2);
        sw = strtoi(ip_src_two, 2) - 1;
        ctrl_id = sw;
        db_id = Get_Ctrl_Conn_Db((uint32_t)ctrl_id, redis_ip);

        // 判断起点属于本区域交换机，向对应控制器发送通告
        // DB_ID = (((inet_addr(redis_ip))&0xff000000)>>24) - 1
        if(db_id == (((inet_addr(redis_ip))&0xff000000)>>24) - 1)
        {
            strncpy(ip_src, reply->element[i]->str, IP_LEN);
            strncpy(ip_dst, reply->element[i]->str + IP_LEN, IP_LEN);
            num = reply->element[i]->str[IP_LEN*2] - '0';
            Del_Rt_Set(slot, ip_src, ip_dst, num, redis_ip);

            strncpy(ip_dst_two, reply->element[i]->str+IP_LEN+6, 2);
            sw1 = strtoi(ip_src_two, 2) - 1;
            sw2 = strtoi(ip_dst_two, 2) - 1;
            
            // 判断是d2d路由 && 源节点是本地数据库 -> 直接通告源节点出端口
            if(((strstr(ip_dst, "44") != NULL) && (strstr(ip_src, "44") != NULL)))
            {
                // 把两条d2d路由的链路映射都删掉
                if(num == 1) Del_Rt_Set(slot, ip_src, ip_dst, 2, redis_ip);
                else Del_Rt_Set(slot, ip_src, ip_dst, 1, redis_ip);

                // Suurballe 计算两点之间的路由
                // 初始化
                printf("\t\t\tstart to run Suurballe\n");
                mindist = 0;
                minnode = -1;
                curnode = -1;
                prenode = -1;
                flag = 0; 
                memset(path, -1, sizeof(path));
                memset(path_new, -1, sizeof(path_new));
                memset(path_1, -1, sizeof(path_1));
                memset(path_2, -1, sizeof(path_2));
                for(a = 0; a < MAX_NUM; a++)
                {
                    node[a][0] = 0;
                    node[a][1] = MAX_DIST;
                    node[a][2] = -1;
                    node[a][3] = -1;
                    node_new[a][0] = 0;
                    node_new[a][1] = MAX_DIST;
                    node_new[a][2] = -1;
                    node_new[a][3] = -1;
                }
                for(a = 0; a < MAX_NUM; a++)
                {
                    for(b = 0; b < MAX_NUM; b++)
                    {
                        matrix_new[a][b] = matrix_ori[a][b];
                    }
                }

                // Dijkstra 计算两点间距离
                node[sw1][0] = 1;
                node[sw1][1] = 0;
                for(a = 0; a < MAX_NUM; a++)
                {
                    if(node[a][0] == 0 && matrix_new[sw1][a] < MAX_DIST)
                    {
                        node[a][1] = matrix_new[sw1][a];
                        node[a][2] = sw1;
                    }
                }
                for(a = 0; a < MAX_NUM; a++)
                {
                    mindist = MAX_DIST;
                    minnode = -1;
                    for(b = 0; b < MAX_NUM; b++)
                    {
                        if(node[b][0] != 0) continue;
                        if(node[b][1] < mindist)
                        {
                            mindist = node[b][1];
                            minnode = b;
                        }
                    }

                    if(minnode == -1) break;
                    if(minnode == sw2) flag = 1;

                    node[minnode][0] = 1;
                    for(b = 0; b < MAX_NUM; b++)
                    {
                        if(node[b][0] == 0 && node[b][1] > matrix_new[minnode][b] + node[minnode][1] && matrix_new[minnode][b] < MAX_DIST)
                        {
                            node[b][1] = matrix_new[minnode][b] + node[minnode][1];
                            node[b][2] = minnode;
                        }
                    }
                }

                // 生成残差图
                if(flag == 1)
                {
                    for(a = 0; a < MAX_NUM; a++)
                    {
                        for(b = 0; b < MAX_NUM; b++)
                        {
                            if(matrix_new[a][b]!=MAX_DIST)
                                matrix_new[a][b] = matrix_new[a][b] + node[a][1] - node[b][1];
                        }
                    }
                    curnode = sw2;
                    prenode = node[curnode][2];
                    c = 0;
                    path[c++] = curnode;
                    while(prenode != -1)
                    {
                        node[prenode][3] = curnode;
                        matrix_new[curnode][prenode] = 0;
                        matrix_new[prenode][curnode] = MAX_DIST;
                        curnode = prenode;
                        prenode = node[curnode][2];
                        path[c++] = curnode;
                    }
                    
                    // printf("db%d - db%d: \n", sw1, sw2);
                    // c = 0;
                    // printf("\tpath: ");
                    // while(path[c] != -1)
                    // {
                    //     printf("%d ", path[c++]);
                    // }
                    // printf("\n");

                    // 第二次Dijkstra计算
                    node_new[sw1][0] = 1;
                    node_new[sw1][1] = 0;
                    for(a = 0; a < MAX_NUM; a++)
                    {
                        if(node_new[a][0] == 0 && matrix_new[sw1][a] < MAX_DIST)
                        {
                            node_new[a][1] = matrix_new[sw1][a];
                            node_new[a][2] = sw1;
                        }
                    }
                    for(a = 0; a < MAX_NUM; a++)
                    {
                        mindist = MAX_DIST;
                        minnode = -1;
                        for(b = 0; b < MAX_NUM; b++)
                        {
                            if(node_new[b][0] != 0) continue;
                            if(node_new[b][1] < mindist)
                            {
                                mindist = node_new[b][1];
                                minnode = b;
                            }
                        }

                        if(minnode == -1) break;
                        if(minnode == sw2) break;

                        node_new[minnode][0] = 1;
                        for(b = 0; b < MAX_NUM; b++)
                        {
                            if(node_new[b][0] == 0 && node_new[b][1] > matrix_new[minnode][b] + node_new[minnode][1] && matrix_new[minnode][b] < MAX_DIST)
                            {
                                node_new[b][1] = matrix_new[minnode][b] + node_new[minnode][1];
                                node_new[b][2] = minnode;
                            }
                        }
                    }
                    if(minnode == sw2)
                    {
                        curnode = sw2;
                        prenode = node_new[curnode][2];
                        c = 0;
                        path_new[c++] = curnode;
                        while(prenode != -1)
                        {
                            node_new[prenode][3] = curnode;
                            curnode = prenode;
                            prenode = node_new[curnode][2];
                            path_new[c++] = curnode;
                        }

                        // c = 0;
                        // printf("\tpath_new: ");
                        // while(path_new[c] != -1)
                        // {
                        //     printf("%d ", path_new[c++]);
                        // }
                        // printf("\n");

                        // 比较两条路径，删除重复部分
                        a = 0;
                        while(path[a+1] != sw1)
                        {
                            b = 0;
                            while(path_new[b+1] != sw1)
                            {
                                if(path_new[b] == path[a+1] && path_new[b+1] == path[a])
                                {
                                    node[path[a+1]][3] = -1;
                                    node_new[path[a]][3] = -1;
                                    // printf("del overlap link sw%d - sw%d\n", path[i+1], path[i]);
                                }
                                b++;
                            }
                            a++;
                        }

                        // 重组路径
                        path_1[0] = sw1;
                        path_1[1] = node[sw1][3];
                        curnode = path_1[1];
                        c = 2;
                        while(curnode != sw2)
                        {
                            if(node[curnode][3] == -1)
                            {
                                path_1[c] = node_new[curnode][3];
                                node_new[curnode][3] = -1;
                            }
                            else
                            {
                                path_1[c] = node[curnode][3];
                                node[curnode][3] = -1;
                            }
                            curnode = path_1[c];
                            c++;
                        }
                        path_2[0] = sw1;
                        path_2[1] = node_new[sw1][3];
                        curnode = path_2[1];
                        c = 2;
                        while(curnode != sw2)
                        {
                            if(node[curnode][3] == -1)
                                path_2[c] = node_new[curnode][3];
                            else
                                path_2[c] = node[curnode][3];
                            curnode = path_2[c];
                            c++;
                        }

                        printf("db%d - db%d: \n", sw1, sw2);
                        c = 0;
                        printf("\tfail_link d2d new path_1: ");
                        while(path_1[c] != -1)
                        {
                            printf("%d ", path_1[c++]);
                        }
                        printf("\n");
                        c = 0;
                        printf("\tfail_link d2d new path_2: ");
                        while(path_2[c] != -1)
                        {
                            printf("%d ", path_2[c++]);
                        }
                        printf("\n\n");

                        // 向数据库写入2条新路由
                        db_write_id = db_write_select(sw2);
                        printf("target_id: %d, db_write_id: %d\n", sw2, db_write_id);
                        memset(&db_write_ip[11], 0, 9);
                        sprintf(&db_write_ip[11], "%d", db_write_id+1);

                        c = 0;
                        while(path_1[c+1] != -1)
                        {
                            snprintf(sw_port, 8, "%03d%03d ", path_1[c], path_1[c+1]);
                            strncpy(out_sw_port + c * 7, sw_port, 7);
                            c++;
                        }
                        Set_Cal_Route(ip_src, ip_dst, 1, out_sw_port, db_write_ip);
                        memset(out_sw_port, 0, CMD_MAX_LENGHT);
                        // 间隔20ms
                        usleep(20000);
                        c = 0;
                        while(path_2[c+1] != -1)
                        {
                            snprintf(sw_port, 8, "%03d%03d ", path_2[c], path_2[c+1]);
                            strncpy(out_sw_port + c * 7, sw_port, 7);
                            c++;
                        }
                        Set_Cal_Route(ip_src, ip_dst, 2, out_sw_port, db_write_ip);
                        memset(out_sw_port, 0, CMD_MAX_LENGHT);

                        // add route to routes set <-> link
                        Add_Rt_Set((uint32_t)path_1[0], (uint32_t)path_1[1], ip_src, ip_dst, 1, db_write_ip);
                        Add_Rt_Set((uint32_t)path_1[0], (uint32_t)path_2[1], ip_src, ip_dst, 2, db_write_ip);
                        // 通告源节点出端口
                        ip_src_thread = malloc(sizeof(char)*(IP_LEN+1));
                        memset(ip_src_thread, 0, sizeof(char));
                        ip_dst_thread = malloc(sizeof(char)*(IP_LEN+1));
                        memset(ip_dst_thread, 0, sizeof(char));
                        strncpy(ip_src_thread, ip_src, IP_LEN);
                        strncpy(ip_dst_thread, ip_dst, IP_LEN);
                        route_notice(db_id, path_1[0], ip_src_thread, ip_dst_thread, path_1[1], path_2[1], redis_ip);
                    }
                    else
                    {
                        printf("\t\t\tfail_link d2d new path_2 failed\n");
                        // 向数据库写入1条新路由
                        db_write_id = db_write_select(sw2);
                        printf("target_id: %d, db_write_id: %d\n", sw2, db_write_id);
                        memset(&db_write_ip[11], 0, 9);
                        sprintf(&db_write_ip[11], "%d", db_write_id+1);

                        c = 0;
                        while(path[c+1] != -1) c++;
                        a = 0;
                        while(c > 0)
                        {
                            snprintf(sw_port, 8, "%03d%03d ", path[c], path[c-1]);
                            strncpy(out_sw_port + a * 7, sw_port, 7);
                            c--;
                            a++;
                        }
                        Set_Cal_Route(ip_src, ip_dst, 1, out_sw_port, db_write_ip);
                        memset(out_sw_port, 0, CMD_MAX_LENGHT);

                        c = 0;
                        while(path[c+1] != -1) c++;
                        // add route to routes set <-> link
                        Add_Rt_Set((uint32_t)path[c], (uint32_t)path[c-1], ip_src, ip_dst, 1, db_write_ip);
                        // 通告源节点出端口
                        ip_src_thread = malloc(sizeof(char)*(IP_LEN+1));
                        memset(ip_src_thread, 0, sizeof(char));
                        ip_dst_thread = malloc(sizeof(char)*(IP_LEN+1));
                        memset(ip_dst_thread, 0, sizeof(char));
                        strncpy(ip_src_thread, ip_src, IP_LEN);
                        strncpy(ip_dst_thread, ip_src, IP_LEN);
                        route_notice(db_id, path[c], ip_src_thread, ip_dst_thread, path[c-1], 999, redis_ip);
                    }
                }
                else
                {
                    printf("\t\t\tfail_link d2d new paths failed\n");
                }
                printf("\t\t\tfinish to run Suurballe\n");              
            }
            else
            {
                // 判断是 正在工作的 c2d控制通道路由
                // 确定源是本区域的控制器，需要判断目的是本地数据库
                if( (strstr(ip_src, "44") == NULL) && (strstr(ip_dst, "44") != NULL) && (sw2 == db_id) )
                {
                   // 关闭tcp套接字，通知控制器切换数据库
                    if(fd_ctl[sw1] != 0)
                    {
                        printf("\t\t\tclose db->ctrl_%02d socket %d\n", sw1, fd_ctl[sw1]);
                        close(fd_ctl[sw1]);
                    }
                }
                // 判断是 正在工作的 d2c控制通道路由
                // 确定源是本地数据库，需要判断目的是本区域控制器
                if( (strstr(ip_src, "44") != NULL) && (strstr(ip_dst, "44") == NULL) && (Get_Ctrl_Conn_Db((uint32_t)sw2, redis_ip) == db_id) )
                {
                    // 关闭tcp套接字，通知控制器切换数据库
                    if(fd_ctl[sw2] != 0)
                    {
                        printf("\t\t\tclose ctrl_%02d->db socket %d\n", sw2, fd_ctl[sw2]);
                        close(fd_ctl[sw2]);
                    }
                }
            
                cfd = fd_ctl[ctrl_id];
                // printf("cfd:%ld\n", cfd);
                // type:1,sw:3,ip_src:8,ip_dst:8,outport:3,port2:3
                memset(buf, 0, BUFSIZE);
                snprintf(buf, BUFSIZE, "%d%03ld%s%s%03d%03d", ROUTE_DEL, sw, ip_src, ip_dst, 0, 999);
                printf("\t\t\tbuf:%s\n",buf);
                ret = send(cfd, buf, BUFSIZE, 0);
                if (ret == -1)
                {
                    print_err("\t\t\tsend route failed", __LINE__, errno);
                    // 发送失败表示控制器断开连接，将 buf 存入对应的待执行结构 wait_exec_X
                    Add_Wait_Exec(ctrl_id, buf, redis_ip);
                }

                // 删除该路由的链路映射
                // Del_Rt_Set(slot, ip_src, ip_dst, 1, redis_ip);

                // 向数据库写入新路由
                // strncpy(ip_dst_two, reply->element[i]->str+IP_LEN+6, 2);
                // sw1 = strtoi(ip_src_two, 2) - 1;
                // sw2 = strtoi(ip_dst_two, 2) - 1;
                if(matrix[sw1][sw2] != MAX_DIST)
                {
                    hop = 0;
                    nextsw[hop++] = sw1;
                    out(sw1, sw2, &route[0][0], nextsw, &hop);
                    nextsw[hop] = sw2;

                    for(j = 0; j < hop; j++)
                    {
                        snprintf(sw_port, 8, "%03d%03d ", nextsw[j], nextsw[j+1]);
                        strncpy(out_sw_port + j * 7, sw_port, 7);
                    }
                    Set_Cal_Route(ip_src, ip_dst, 1, out_sw_port, redis_ip);
                    memset(out_sw_port, 0, CMD_MAX_LENGHT);
                }
            }
        }
    }

    freeReplyObject(reply);
    redisFree(context);
    free(obj);
    return 0;
}

// 订阅回调函数
void psubCallback(redisAsyncContext *c, void *r, void *redis_ip) 
{
    int i = 0;
    redisReply *reply = (redisReply*)r;
    char *reply_str = malloc(sizeof(char)*26);
    for(i = 0; i < 26; i++)
    {
        reply_str[i] = '\0';
    }
    if(reply == NULL) 
    {
        printf("reply == NULL\n");
        return;
    }
        
    char ip_two[3] = {0,}; // redis_ip最后两位
    int redis_id = -1;
    char slot_two[3] = {0,}; // fail_link最后两位
    int slot_id = -1;

    int ctrl_id = 0;  // 记录控制器ID
    char ctrl_str[3] = {0,};
    int db_id = 0;
    long cfd = -1;
    int ret = -1;
    int index = -1;
    char buf[BUFSIZE] = {0,};
    char cmd[CMD_MAX_LENGHT] = {0};
    redisContext *context1;
    redisReply *reply1;

    // 订阅接收到的消息是一个带三元素的数组
    if(reply->type == REDIS_REPLY_ARRAY && reply->elements == 3) 
    {
        if (strcmp( reply->element[0]->str, "psubscribe") == 0) 
        {
            printf( "Received[%s] channel %s: %s\n",
                    "psub",
                    reply->element[1]->str,
                    reply->element[2]->str );
        }
    }
    // 订阅接收到的消息是一个带四元素的数组
    else if(reply->type == REDIS_REPLY_ARRAY && reply->elements == 4)
    {
        // printf("Recieved message: %s -- %s\n", 
        //         reply->element[2]->str,
        //         reply->element[3]->str);

        // 判断操作是否为rpush
        if(strstr(reply->element[2]->str, "rpush") != NULL)
        {
            if(strstr(reply->element[3]->str, "calrt") != NULL)
            {
                printf("Recieved message: %s -- %s\n", 
                reply->element[2]->str,
                reply->element[3]->str);

                // 查询数据库，下发流表项
                strncpy(reply_str, reply->element[3]->str, 24);
                if(route_add(reply_str, redis_ip) == -1)
                    printf("cal route %s add failure\n", reply->element[3]->str);
                else
                    printf("cal route %s add success\n", reply->element[3]->str);
            }
            // 判断是fail_link
            else if(strstr(reply->element[3]->str, "fail_link") != NULL) 
            {
                printf("Recieved message: %s -- %s\n", 
                reply->element[2]->str,
                reply->element[3]->str);
                
                // 查询数据库，下发流表项
                strncpy(ip_two, reply->element[3]->str+10, 2);
                redis_id = atoi(ip_two);
                strncpy(reply_str, reply->element[3]->str, 24);

                strncpy(slot_two, reply->element[3]->str+13, 2);
                slot_id = atoi(slot_two);
                index = fail_link_index[slot_id][redis_id];
                fail_link_index[slot_id][redis_id]++;
                printf("redis_id = %d, index = %d\n", redis_id, index);
                if(route_del(reply_str, index, redis_ip) == -1)
                    printf("%s route del failure\n", reply->element[3]->str);
                else
                    printf("%s route del success\n", reply->element[3]->str);
            }
        }
        // 判断操作是否为sadd
        else if(strstr(reply->element[2]->str, "sadd") != NULL)
        {
            if(strstr(reply->element[3]->str, "wait_exec") != NULL)
            {
                printf("Recieved message: %s -- %s\n", 
                reply->element[2]->str,
                reply->element[3]->str);
                
                strncpy(ctrl_str, reply->element[3]->str+10, 2);
                ctrl_id = atol(ctrl_str);
                db_id = Get_Ctrl_Conn_Db((uint32_t)ctrl_id, redis_ip);

                // 判断起点属于本区域交换机，向对应控制器发送通告
                // DB_ID = (((inet_addr(redis_ip))&0xff000000)>>24) - 1
                if(db_id == (((inet_addr(redis_ip))&0xff000000)>>24) - 1)
                {
                    cfd = fd_ctl[ctrl_id];

                    // 查询数据库，下发流表项
                    snprintf(cmd, CMD_MAX_LENGHT, "smembers %s", reply->element[3]->str);
                    context1 = redisConnect(redis_ip, REDIS_SERVER_PORT);
                    if (context1->err)
                    {
                        printf("Error: %s\n", context1->errstr);
                        redisFree(context1);
                        return;
                    }
                    // printf("connect redis server success\n");
                    reply1 = (redisReply *)redisCommand(context1, cmd);
                    if (reply1 == NULL)
                    {
                        printf("execute command:%s failure\n", cmd);
                        redisFree(context1);
                        return;
                    }

                    // 输出查询结果
                    printf("%s entry num = %lu\n", reply->element[3]->str, reply1->elements);
                    if(reply1->elements == 0) 
                    {
                        freeReplyObject(reply1);
                        redisFree(context1);
                        return;
                    }
                    for(i = 0; i < reply1->elements; i++)
                    {
                        printf("\tctrl_%d buf_%d: %s\n", ctrl_id, i, reply1->element[i]->str);
                        memset(buf, 0, BUFSIZE);
                        snprintf(buf, BUFSIZE, "%s", reply1->element[i]->str);
                        ret = send(cfd, buf, BUFSIZE, 0);
                        if (ret == -1)
                        {
                            print_err("send route failed", __LINE__, errno);
                        }
                        else
                        {
                            // 成功下发通告之后，删除相应的wait_exec元素
                            Del_Wait_Exec(ctrl_id, buf, redis_ip);
                        }
                    }
                    
                    freeReplyObject(reply1);
                    redisFree(context1);
                }
            }
        }
    }
    else
        printf("reply->type: %d, reply->elements: %d\n", (int)reply->type, (int)reply->elements);
}

// 远程连接回调函数
void connectCallback(const redisAsyncContext *c, int status) 
{
    if (status != REDIS_OK) 
    {
        printf("Error: %s\n", c->errstr);
        return;
    }
    printf("Connected...\n");
}

// 断开连接回调函数
void disconnectCallback(const redisAsyncContext *c, int status) 
{
    if (status != REDIS_OK) 
    {
        printf("Error: %s\n", c->errstr);
        return;
    }
    printf("Disconnected...\n");
}

int main(int argc, char **argv) 
{
    long skfd = -1, ret = -1;
    char *redis_ip = *++argv;
    printf("redis_ip: %s\n", redis_ip);
    int local_db_id = (((inet_addr(redis_ip))&0xff000000)>>24) -1;
    printf("local_db_id: %d\n", local_db_id);

    memset(fd_db_client, -1, sizeof(fd_db_client));
    memset(fd_db_server, -1, sizeof(fd_db_server));
    memset(db_neighbor_list, -1, sizeof(db_neighbor_list));

    // 读取数据库邻居信息
    printf("读取数据库邻居信息\n");
    FILE *fp = NULL;
    char ip_addr[20] = {0, };
    int db_id = -1;
    fp = fopen("/home/config/db_conf/db_ip_file", "r");
    long i = 0;
    int j = 0;
    while(fscanf(fp, "%s", ip_addr) != EOF)
    {
        db_id = (((inet_addr(ip_addr))&0xff000000)>>24) -1;
        if(db_id != local_db_id)
        {
            db_neighbor_list[j++] = db_id;
        }
    }
    
    // 初始化TCP套接字
    printf("初始化TCP套接字\n");
	skfd = socket(AF_INET, SOCK_STREAM, 0);
	if (skfd == -1) 
    {
		print_err("socket failed",__LINE__,errno);
	}
	struct sockaddr_in addr_1;
	addr_1.sin_family = AF_INET; // 设置tcp协议族
	addr_1.sin_port = htons(SERVER_PORT); // 设置端口号
	addr_1.sin_addr.s_addr = inet_addr(redis_ip); // 设置ip地址
	ret = bind(skfd, (struct sockaddr*)&addr_1, sizeof(addr_1));
	if (ret == -1) 
    {
        print_err("bind failed",__LINE__,errno);
	}
	ret = listen(skfd, 30);
    if (ret == -1) 
    {
        print_err("listen failed", __LINE__, errno);
	}

    server_fd_tcp = socket(AF_INET, SOCK_STREAM, 0);
	if (server_fd_tcp == -1) 
    {
		print_err("socket failed",__LINE__,errno);
	}
	struct sockaddr_in addr_2;
	addr_2.sin_family = AF_INET; // 设置tcp协议族
	addr_2.sin_port = htons(TCP_SERVER_PORT); // 设置端口号
	addr_2.sin_addr.s_addr = inet_addr(redis_ip); // 设置ip地址
	ret = bind(server_fd_tcp, (struct sockaddr*)&addr_2, sizeof(addr_2));
	if (ret == -1) 
    {
        print_err("bind failed",__LINE__,errno);
	}
	ret = listen(server_fd_tcp, 10);
    if (ret == -1) 
    {
        print_err("listen failed", __LINE__, errno);
	}
	
	pthread_t tcpid_1, tcpid_2, tcpid_3, tcpid_4, udpid;
	// 创建子线程，使用accept阻塞监听客户端的连接
    printf("创建子线程，使用accept阻塞监听客户端的连接\n");
    ret = pthread_create(&tcpid_1, NULL, tcpconnect_ctl, (void*)skfd);
    if (ret == -1) 
    {
        print_err("create tcpconnect_ctl failed", __LINE__, errno); 
    }
    ret = pthread_create(&tcpid_2, NULL, tcpconnect_db, NULL);
    if (ret == -1) 
    {
        print_err("create tcpconnect_db failed", __LINE__, errno); 
    }

    // 创建子线程，主动连接数据库
    printf("创建子线程，主动连接数据库\n");
    for(i = 0; i < DB_NUM-1; i++)
    {
        ret = pthread_create(&tcpid_3, NULL, tcpconnect_client, (void*)i);
        if (ret == -1) 
        {
            print_err("create tcpconnect_client failed", __LINE__, errno); 
        }
    }
    // 创建子线程，监听数据库连接状态
    printf("创建子线程，监听数据库连接状态\n");
    ret = pthread_create(&tcpid_4, NULL, keep_alive, NULL);
    if (ret == -1) 
    {
        print_err("create tcpconnect_client failed", __LINE__, errno); 
    }

    //创建子线程，获取时间片序号slot
    printf("创建子线程，获取时间片序号slot\n");
    ret = pthread_create(&udpid, NULL, udpconnect, redis_ip);
    if (ret == -1) 
    {
        print_err("create udpconnect failed", __LINE__, errno); 
    }

    signal(SIGPIPE, SIG_IGN);
    struct event_base *base = event_base_new(); // 创建libevent对象 alloc并返回一个带默认配置的event base

    redisAsyncContext *c = redisAsyncConnect(redis_ip, REDIS_SERVER_PORT);
    if (c->err) 
    {
        printf("Error: %s\n", c->errstr);
        return -1;
    }

    redisLibeventAttach(c,base); // 将事件绑定到redis context上，使设置给redis的回调跟事件关联

    redisAsyncSetConnectCallback(c,connectCallback); // 设置连接回调，当异步调用连接后，服务器处理连接请求结束后调用，通知调用者连接的状态
    redisAsyncSetDisconnectCallback(c,disconnectCallback); // 设置断开连接回调，当服务器断开连接后，通知调用者连接断开，调用者可以利用这个函数实现重连
    redisAsyncCommand(c, psubCallback, redis_ip, "psubscribe __key*__:*");

    // 开启事件分发，event_base_dispatch会阻塞
    event_base_dispatch(base); // 运行event_base，直到没有event被注册在event_base中为止

    printf("quit!\n");
    return 0;
}
