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
#include "db_wr.h"
#include "db_wr.c"

#define MAX_NUM 66
#define CMD_MAX_LENGHT 256
#define REDIS_SERVER_IP "192.168.10.118"

#define REDIS_SERVER_PORT 8102

#define DB_ID 2 // database_id = 192.168.68.2
#define SERVER_IP "127.0.0.1" // tcp+udp ip
#define SERVER_PORT 2345 // tcp port
#define UDP_PORT 12000 // udp port
#define BUFSIZE 512
#define ROUTE_ADD 1 // type_1 add
#define ROUTE_DEL 2 // type_2 del
#define CAL_FAIL 0
#define CAL_SUCCESS 1
#define GOTO_TABLE 255
#define IP_LEN 8

int fd[MAX_NUM] = {0, }; // 记录不同控制器节点对应的套接字描述符
int slot = 0; // slot_id
int fail_link_index = 0; // 记录已经处理到的fail_link列表的索引
int server_fd = -1; // UDP监听的套接字

void print_err(char *str, int line, int err_no) {
	printf("%d, %s :%s\n",line,str,strerror(err_no));
	// _exit(-1);
}

int listen_init(void)
{
    // 初始化监听套接字
    int ret;
    struct sockaddr_in ser_addr;

    server_fd = socket(AF_INET, SOCK_DGRAM, 0); //AF_INET:IPV4;SOCK_DGRAM:UDP
    if(server_fd < 0)
    {
        printf("create socket fail!\n");
        return -1;
    }

    memset(&ser_addr, 0, sizeof(ser_addr));
    ser_addr.sin_family = AF_INET;
    ser_addr.sin_addr.s_addr = inet_addr(SERVER_IP); //IP地址，需要进行网络序转换，INADDR_ANY：本地地址
    ser_addr.sin_port = htons(UDP_PORT);  //端口号，需要网络序转换

    ret = bind(server_fd, (struct sockaddr*)&ser_addr, sizeof(ser_addr));
    if(ret < 0)
    {
        printf("socket bind fail!\n");
        return -1;
    }
    return 0;
}

// 子线程中发送一个消息给客户端
// void *sendmessage(void *pth_arg)
// {
// 	int ret = 0;
// 	long cfd = (long)pth_arg; // client fd
// 	char buf[100] = {0};
// 	while(1) 
//     {
// 		bzero(&buf, sizeof(buf));
// 		// ret = recv(cfd, &buf, sizeof(buf), 0);	
// 		// if (ret == -1)
//         // {
// 		// 	print_err("recv failed",__LINE__,errno);
//         // }
// 		// else if(ret > 0)
// 		// 	printf("recv from client %s \n", buf);
// 		// ret = send(cfd, "recv ok\n", sizeof("recv ok\n"), 0);
        
//         ret = send(cfd, &buf, sizeof(buf), 0);
// 		if (ret == -1)
//         {
//             print_err("send failed", __LINE__, errno);
//         }
// 	}
// }

// 子线程中等待客户端连接
void *tcpconnect(void *pth_arg)
{
	long skfd = (long)pth_arg;

    // 使用accept阻塞形式得监听客户端的发来的连接，并返回通信描述符
	long cfd = -1;
	pthread_t id;
	while(1) 
    {
		struct sockaddr_in caddr = {0};
		int csize = sizeof(caddr);
		cfd = accept(skfd, (struct sockaddr*)&caddr, &csize);
		if (cfd == -1) 
        {
			print_err("accept failed", __LINE__, errno);
		}
		// 建立连接后打印一下客户端的ip和端口号
		printf("cport = %d, caddr = %s\n", ntohs(caddr.sin_port),inet_ntoa(caddr.sin_addr));
        // printf("ctrl_id = %d\n", ((inet_addr(inet_ntoa(caddr.sin_addr)))&0xff000000)>>24);
        // 记录SDN控制器对应的套接字描述符
        fd[(((inet_addr(inet_ntoa(caddr.sin_addr)))&0xff000000)>>24)] = cfd;

	}
}

void *work_thread(void *pth_arg)
{
    // 校对topo将失效链路加入fail_link
    Diff_Topo(slot, REDIS_SERVER_IP);

/****************************************************************************/
    // 根据del_link遍历路由条目调整定时
    char cmd[CMD_MAX_LENGHT] = {0};
    redisContext *context1, *context2;
    redisReply *reply1, *reply2;
    uint64_t sw;
    uint32_t sw1, sw2;
    int i = 0;
    int ctrl_id = 0; // 记录控制器ID
    int db_id = 0;
    long cfd = -1;
    int ret = -1;
    char buf[BUFSIZE] = {0,};
    char ip_src[IP_LEN] = {0,};
    char ip_dst[IP_LEN] = {0,};

    /*组装Redis命令*/
    snprintf(cmd, CMD_MAX_LENGHT, "smembers del_link_%02d", slot);

    /*连接redis*/
    context1 = redisConnect(REDIS_SERVER_IP, REDIS_SERVER_PORT);
    if (context1->err)
    {
        redisFree(context1);
        printf("Error: %s\n", context1->errstr);
        return NULL;
    }
    printf("connect redis server success\n");

    /*执行redis命令*/
    reply1 = (redisReply *)redisCommand(context1, cmd);
    if (reply1 == NULL)
    {
        printf("execute command:%s failure\n", cmd);
        redisFree(context1);
        return NULL;
    }

    // 输出查询结果
    printf("\tentry num = %lu\n",reply1->elements);
    if(reply1->elements == 0) return NULL;
    for(i = 0; i < reply1->elements; i++)
    {
        sw = atol(reply1->str);
        sw1 = (uint32_t)((sw & 0xffffffff00000000) >> 32);
        sw2 = (uint32_t)(sw & 0x00000000ffffffff);
        printf("\tdel_link: sw%02d<->sw%02d\n", sw1, sw2);

        ctrl_id = Get_Active_Ctrl((uint32_t)sw1, slot, REDIS_SERVER_IP);
        if(Lookup_Sw_Set((uint32_t)ctrl_id, (uint32_t)sw1, slot, REDIS_SERVER_IP) == FAILURE)
        {
            ctrl_id = Get_Standby_Ctrl((uint32_t)sw1, slot, REDIS_SERVER_IP);
        }
        db_id = Get_Ctrl_Conn_Db((uint32_t)ctrl_id, slot, REDIS_SERVER_IP);

        // 判断起点属于本区域交换机，查询相关的非定时路由，向对应控制器发送定时通告
        if(db_id == DB_ID)
        {
            // 查询相关的非定时路由
            /*组装Redis命令*/
            snprintf(cmd, CMD_MAX_LENGHT, "smembers rt_set_%02d_%02d", sw1, sw2);

            /*连接redis*/
            context2 = redisConnect(REDIS_SERVER_IP, REDIS_SERVER_PORT);
            if (context2->err)
            {
                redisFree(context2);
                printf("Error: %s\n", context2->errstr);
                continue;
            }
            printf("connect redis server success\n");

            /*执行redis命令*/
            reply2 = (redisReply *)redisCommand(context2, cmd);
            if (reply2 == NULL)
            {
                printf("execute command:%s failure\n", cmd);
                redisFree(context2);
                continue;
            }

            // 输出查询结果
            printf("\tentry num = %lu\n",reply2->elements);
            if(reply2->elements == 0) continue;
            for(i = 0; i < reply2->elements; i++)
            {
                printf("\troute entry: %s\n",reply2->element[i]->str);

                // 向对应控制器发送定时通告
                cfd = fd[ctrl_id];
                // type:1,sw:3,ip_src:8,ip_dst:8,outport:3,timeout:3
                bzero(&buf, sizeof(buf));
                snprintf(buf, BUFSIZE, "%d%03ld%s%03d%03d", ROUTE_ADD, sw, reply2->element[i]->str, 0, (SLOT_TIME - SLOT_TIME/4));
                ret = send(cfd, buf, sizeof(buf), 0);
                if (ret == -1)
                {
                    print_err("send route failed", __LINE__, errno);
                }

                strncpy(ip_src, reply2->element[i]->str, IP_LEN);
                strncpy(ip_dst, reply2->element[i]->str + IP_LEN, IP_LEN);
                Mov_Rt_Set((uint32_t)sw1, (uint32_t)sw2, slot, ip_src, ip_dst, REDIS_SERVER_IP);
            }

            freeReplyObject(reply2);
            redisFree(context2);
        }
    }

    freeReplyObject(reply1);
    redisFree(context1);
    return NULL;
}

void *udpconnect(void *pth_arg)
{
    uint8_t buf[BUFSIZE] = {'\0'};
    struct sockaddr_in *clent_addr;
    socklen_t len = sizeof(struct sockaddr_in);
    pthread_t pid;
    long ret = -1;
    
    if(listen_init() != 0)
    {
        printf("套接字初始化失败\n"); 
        return NULL;
    }
    while(1)
    {
        bzero(&buf, sizeof(buf));
        recvfrom(server_fd, buf, BUFSIZE, 0, (struct sockaddr*)clent_addr, &len);
        slot = atoi(buf);
        fail_link_index = 0;

        // wait converge
        sleep(SLOT_TIME/4);
        //创建子线程，校对topo将失效链路加入fail_link，并根据del_link遍历路由条目调整定时
        ret = pthread_create(&pid, NULL, work_thread, NULL);
        if (ret == -1) 
        {
            print_err("create work_thread failed", __LINE__, errno); 
        }
    }
}

// 向相应的控制器发送新增路由表项
int route_add(char *obj, int flag)
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

    // char slot_str[2] = {0,};
    // strncpy(slot_str, obj+14, 2);
    // int slot = atoi(slot_str);

    char ip_src[IP_LEN] = {0,};
    char ip_dst[IP_LEN] = {0,};
    int timeout = 0;
    strncpy(ip_src, obj + 6, IP_LEN);
    strncpy(ip_dst, obj + 6 + IP_LEN, IP_LEN);

    /*组装Redis命令*/
    snprintf(cmd, CMD_MAX_LENGHT, "lrange %s 0 -1", obj);

    /*连接redis*/
    context = redisConnect(REDIS_SERVER_IP, REDIS_SERVER_PORT);
    if (context->err)
    {
        redisFree(context);
        printf("Error: %s\n", context->errstr);
        return -1;
    }
    printf("connect redis server success\n");

    /*执行redis命令*/
    reply = (redisReply *)redisCommand(context, cmd);
    if (reply == NULL)
    {
        printf("execute command:%s failure\n", cmd);
        redisFree(context);
        return -1;
    }

    // 输出查询结果
    printf("\tentry num = %lu\n",reply->elements);
    if(reply->elements == 0) return -1;
    for(i = 0; i < reply->elements; i++)
    {
        printf("\tout_sw_port: %s\n",reply->element[i]->str);
        sw = atoi(reply->element[i]->str)/1000;
        port = atoi(reply->element[i]->str)%1000;
        ctrl_id = Get_Active_Ctrl((uint32_t)sw, slot, REDIS_SERVER_IP);
        if(Lookup_Sw_Set((uint32_t)ctrl_id, (uint32_t)sw, slot, REDIS_SERVER_IP) == FAILURE)
        {
            ctrl_id = Get_Standby_Ctrl((uint32_t)sw, slot, REDIS_SERVER_IP);
        }
        db_id = Get_Ctrl_Conn_Db((uint32_t)ctrl_id, slot, REDIS_SERVER_IP);

        // 判断该出端口属于本区域交换机，向对应控制器发送通告
        if(db_id == DB_ID)
        {
            cfd = fd[ctrl_id]; 
            if(flag == FAILURE)
            {
                timeout = 5;
                port = GOTO_TABLE; // 暂时采用5s作为定时
            }
            else
            {
                if(Lookup_Del_Link((uint32_t)sw, (uint32_t)port, slot, REDIS_SERVER_IP) == SUCCESS)
                {
                    timeout = SLOT_TIME; // 暂时采用时间片长度作为定时
                    // add route to routes set <-> link
                    Add_Rt_Set_Time((uint32_t)sw, (uint32_t)port, slot, ip_src, ip_dst, REDIS_SERVER_IP);
                }
                else
                {
                    timeout = 0;
                    // add route to routes set <-> link
                    Add_Rt_Set((uint32_t)sw, (uint32_t)port, ip_src, ip_dst, REDIS_SERVER_IP);
                }               
            }

            // type:1,sw:3,ip_src:8,ip_dst:8,outport:3,timeout:3
            bzero(&buf, sizeof(buf));
            snprintf(buf, BUFSIZE, "%d%03d%s%s%03d%03d", ROUTE_ADD, sw, ip_src, ip_dst, port, timeout);
            ret = send(cfd, buf, sizeof(buf), 0);
            if (ret == -1)
            {
                print_err("send route failed", __LINE__, errno);
            }

            // ret = send(cfd, obj, sizeof(obj), 0);
            // if (ret == -1)
            // {
            //     print_err("send route entry failed", __LINE__, errno);
            // }
            // ret = send(cfd, reply->element[i]->str, sizeof(reply->element[i]->str), 0);
            // if (ret == -1)
            // {
            //     print_err("send switch outport failed", __LINE__, errno);
            // }
            // printf("send over\n");
        }
    }

    freeReplyObject(reply);
    redisFree(context);
    return 0;
}

// 向相应的控制器发送删除路由表项
int route_del(char *obj, int index)
{
    char cmd[CMD_MAX_LENGHT] = {0};
    redisContext *context;
    redisReply *reply;
    int i = 0;
    int ctrl_id = 0; // 记录控制器ID
    int db_id = 0;
    uint32_t sw1, sw2 = 0;
    uint64_t sw = 0;
    int port = 0;
    long cfd = -1;
    int ret = -1;
    char buf[BUFSIZE] = {0,};
    char ip_src[IP_LEN] = {0,};
    char ip_dst[IP_LEN] = {0,};
    char ip_src_two[IP_LEN/4] = {0,}; // ip_src最后两位

    char slot_str[2] = {0,};
    strncpy(slot_str, obj+10, 2);
    int slot = atoi(slot_str);

    /*组装Redis命令*/
    snprintf(cmd, CMD_MAX_LENGHT, "lindex %s %d", obj, index);

    /*连接redis*/
    context = redisConnect(REDIS_SERVER_IP, REDIS_SERVER_PORT);
    if (context->err)
    {
        redisFree(context);
        printf("Error: %s\n", context->errstr);
        return -1;
    }
    printf("connect redis server success\n");

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
        return -1;
    }
    sw = atol(reply->str);
    sw1 = (uint32_t)((sw & 0xffffffff00000000) >> 32);
    sw2 = (uint32_t)(sw & 0x00000000ffffffff);
    printf("\tfail_link: sw%d<->sw%d\n",sw1, sw2);

    /*组装Redis命令*/
    snprintf(cmd, CMD_MAX_LENGHT, "smembers rt_set_%02d_%02d", sw1, sw2);

    /*连接redis*/
    context = redisConnect(REDIS_SERVER_IP, REDIS_SERVER_PORT);
    if (context->err)
    {
        redisFree(context);
        printf("Error: %s\n", context->errstr);
        return -1;
    }
    printf("connect redis server success\n");

    /*执行redis命令*/
    reply = (redisReply *)redisCommand(context, cmd);
    if (reply == NULL)
    {
        printf("execute command:%s failure\n", cmd);
        redisFree(context);
        return -1;
    }

    // 输出查询结果
    printf("\tentry num = %lu\n",reply->elements);
    if(reply->elements == 0) return -1;
    for(i = 0; i < reply->elements; i++)
    {
        printf("\troute entry: %s\n",reply->element[i]->str);
        strncpy(ip_src_two, reply->element[i]->str+6, 2);
        sw = atol(ip_src_two)-1;
        ctrl_id = Get_Active_Ctrl((uint32_t)sw, slot, REDIS_SERVER_IP);
        if(Lookup_Sw_Set((uint32_t)ctrl_id, (uint32_t)sw, slot, REDIS_SERVER_IP) == FAILURE)
        {
            ctrl_id = Get_Standby_Ctrl((uint32_t)sw, slot, REDIS_SERVER_IP);
        }
        db_id = Get_Ctrl_Conn_Db((uint32_t)ctrl_id, slot, REDIS_SERVER_IP);

        // 判断起点属于本区域交换机，向对应控制器发送通告
        if(db_id == DB_ID)
        {
            cfd = fd[ctrl_id];

            // type:1,sw:3,ip_src:8,ip_dst:8,outport:3,timeout:3
            bzero(&buf, sizeof(buf));
            snprintf(buf, BUFSIZE, "%d%03ld%s%03d%03d", ROUTE_DEL, sw, reply->element[i]->str, 0, 0);
            ret = send(cfd, buf, sizeof(buf), 0);
            if (ret == -1)
            {
                print_err("send route failed", __LINE__, errno);
            }

            strncpy(ip_src, reply->element[i]->str, IP_LEN);
            strncpy(ip_dst, reply->element[i]->str + IP_LEN, IP_LEN);
            Del_Rt_Set(slot, ip_src, ip_dst, REDIS_SERVER_IP);
        }
    }

    freeReplyObject(reply);
    redisFree(context);
    return 0;
}

// 订阅回调函数
void psubCallback(redisAsyncContext *c, void *r, void *priv) 
{
    int i = 0;
    redisReply *reply = (redisReply*)r;
    if (reply == NULL) return;

    // 订阅接收到的消息是一个带三元素的数组
    if (reply->type == REDIS_REPLY_ARRAY && reply->elements == 3) 
    {
        if (strcmp( reply->element[0]->str, "psubscribe") == 0) 
        {
            printf( "Received[%s] channel %s: %s\n",
                    (char*)priv,
                    reply->element[1]->str,
                    reply->element[2]->str );
        }
    }

    // 订阅接收到的消息是一个带四元素的数组
    if (reply->type == REDIS_REPLY_ARRAY && reply->elements == 4)
    {
        printf("Recieved message:\n\t(1)channel: %s\n\t(2)option: %s\n\t(3)object: %s\n", 
                reply->element[1]->str,
                reply->element[2]->str,
                reply->element[3]->str);

        // 判断操作是否为rpush
        if(strstr(reply->element[2]->str, "rpush") != NULL)
        {
            // 查询数据库，下发流表项
            if(strstr(reply->element[3]->str, "calrt") != NULL)
            {
                if(route_add(reply->element[3]->str, CAL_SUCCESS) == -1)
                    printf("cal route add failure\n");
                else
                    printf("cal route add success\n");
            }
            else if(strstr(reply->element[3]->str, "failrt") != NULL)
            {
                // failrt_%s%s => dflrt_%s%s_%02d
                reply->element[3]->str[1] = 'd';
                reply->element[3]->str[2] = 'f';
                snprintf(reply->element[3]->str + 23, BUFSIZE, "_%02d", slot);
                if(route_add(reply->element[3]->str + 1, CAL_FAIL) == -1)
                    printf("dfl route add failure\n");
                else
                    printf("dfl route add success\n");
            }
            else if(strstr(reply->element[3]->str, "fail_link") != NULL)
            {
                if(route_del(reply->element[3]->str, fail_link_index) == -1)
                    printf("route del failure\n");
                else
                    printf("route del success\n");
                fail_link_index++;
            }
        }
    }
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
	skfd = socket(AF_INET, SOCK_STREAM, 0);
	if (skfd == -1) 
    {
		print_err("socket failed",__LINE__,errno);
	}

	struct sockaddr_in addr;
	addr.sin_family = AF_INET; // 设置tcp协议族
	addr.sin_port = htons(SERVER_PORT); // 设置端口号
	addr.sin_addr.s_addr = inet_addr(SERVER_IP); // 设置ip地址

	ret = bind(skfd, (struct sockaddr*)&addr, sizeof(addr));
	if (ret == -1) 
    {
        print_err("bind failed",__LINE__,errno);
	}
 
	ret = listen(skfd, 10);
    if (ret == -1) 
    {
        print_err("listen failed", __LINE__, errno);
	}
	
	pthread_t tcpid, udpid;
	//创建子线程，使用accept阻塞监听客户端的连接
    ret = pthread_create(&tcpid, NULL, tcpconnect, (void*)skfd);
    if (ret == -1) 
    {
        print_err("create tcpconnect failed", __LINE__, errno); 
    }
    //创建子线程，获取时间片序号slot
    ret = pthread_create(&udpid, NULL, udpconnect, NULL);
    if (ret == -1) 
    {
        print_err("create udpconnect failed", __LINE__, errno); 
    }


    signal(SIGPIPE, SIG_IGN);
    struct event_base *base = event_base_new(); // 创建libevent对象 alloc并返回一个带默认配置的event base

    redisAsyncContext *c = redisAsyncConnect(REDIS_SERVER_IP, REDIS_SERVER_PORT);
    if (c->err) 
    {
        printf("Error: %s\n", c->errstr);
        return -1;
    }

    redisLibeventAttach(c,base); // 将事件绑定到redis context上，使设置给redis的回调跟事件关联

    redisAsyncSetConnectCallback(c,connectCallback); // 设置连接回调，当异步调用连接后，服务器处理连接请求结束后调用，通知调用者连接的状态
    redisAsyncSetDisconnectCallback(c,disconnectCallback); // 设置断开连接回调，当服务器断开连接后，通知调用者连接断开，调用者可以利用这个函数实现重连
    redisAsyncCommand(c, psubCallback, (char*) "psub", "psubscribe __key*__:*");

    // 开启事件分发，event_base_dispatch会阻塞
    event_base_dispatch(base); // 运行event_base，直到没有event被注册在event_base中为止

    return 0;
}
