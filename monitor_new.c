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
#define CMD_MAX_LENGHT 256
// #define REDIS_SERVER_IP "192.168.10.118"

#define REDIS_SERVER_PORT 6379

// #define DB_ID 2 // database_id = 192.168.68.2
// #define SERVER_IP "127.0.0.1" // tcp+udp ip
#define SERVER_PORT 2345 // tcp port
#define UDP_PORT 12000 // udp port
#define BUFSIZE 512
#define ROUTE_ADD 1 // type_1 add
#define ROUTE_DEL 2 // type_2 del
#define CAL_FAIL 0
#define CAL_SUCCESS 1
#define GOTO_TABLE 255
#define IP_LEN 8
#define MAX_DIST 0x3f3f3f3f

int fd[MAX_NUM] = {0, }; // 记录不同控制器节点对应的套接字描述符
int slot = 0; // slot_id
int fail_link_index[MAX_NUM] = {0, }; // 记录已经处理到的fail_link列表的索引
int server_fd = -1; // UDP监听的套接字

void print_err(char *str, int line, int err_no) {
	printf("%d, %s :%s\n",line,str,strerror(err_no));
	// _exit(-1);
}

int listen_init(char *redis_ip)
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
    ser_addr.sin_addr.s_addr = inet_addr(redis_ip); //IP地址，需要进行网络序转换，INADDR_ANY：本地地址
    ser_addr.sin_port = htons(UDP_PORT);  //端口号，需要网络序转换

    ret = bind(server_fd, (struct sockaddr*)&ser_addr, sizeof(ser_addr));
    if(ret < 0)
    {
        printf("socket bind fail!\n");
        close(server_fd);
        return -1;
    }
    return 0;
}

// 子线程中等待客户端连接
void *tcpconnect(void *pth_arg)
{
	long skfd = (long)pth_arg;

    // 使用accept阻塞形式得监听客户端的发来的连接，并返回通信描述符
	long cfd = -1;
    int ctrl_id = -1;
	pthread_t id;
    int keepAlive = 1; // 开启keepalive属性
    int keepIdle = 1; // 如该连接在1秒内没有任何数据往来,则进行探测 
    int keepInterval = 1; // 探测时发包的时间间隔为1 秒
    int keepCount = 1; // 探测尝试的次数

	while(1) 
    {
		struct sockaddr_in caddr = {0};
		int csize = sizeof(caddr);
		cfd = accept(skfd, (struct sockaddr*)&caddr, &csize);
        setsockopt(skfd, SOL_SOCKET, SO_KEEPALIVE, (void *)&keepAlive, sizeof(keepAlive));
        setsockopt(skfd, SOL_TCP, TCP_KEEPIDLE, (void*)&keepIdle, sizeof(keepIdle));
        setsockopt(skfd, SOL_TCP, TCP_KEEPINTVL, (void *)&keepInterval, sizeof(keepInterval));
        setsockopt(skfd, SOL_TCP, TCP_KEEPCNT, (void *)&keepCount, sizeof(keepCount));
		if (cfd == -1) 
        {
			print_err("accept failed", __LINE__, errno);
		}
		// 建立连接后打印一下客户端的ip和端口号
		printf("cport = %d, caddr = %s\n", ntohs(caddr.sin_port),inet_ntoa(caddr.sin_addr));
        // printf("ctrl_id = %d\n", ((inet_addr(inet_ntoa(caddr.sin_addr)))&0xff000000)>>24);
        // 记录SDN控制器对应的套接字描述符
        ctrl_id = (((inet_addr(inet_ntoa(caddr.sin_addr)))&0xff000000)>>24) -1;
        if(fd[ctrl_id] != 0) close(fd[ctrl_id]);
        fd[ctrl_id] = cfd;
	}
}

// 用于输出路径，并写入文件
void out(int node1, int node2, int *route, int *nextsw, int *hop)
{
    if(*(route + node1*MAX_NUM + node2) == -1)
        return;
    out(node1, *(route + node1*MAX_NUM + node2), route, nextsw, hop);
    nextsw[(*hop)++] = *(route + node1*MAX_NUM + node2);
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
    int i, j, k = 0;
    int ctrl_id = 0; // 记录控制器ID
    int db_id = 0;
    long cfd = -1;
    int ret = -1;
    char buf[BUFSIZE] = {0,};
    char ip_src[IP_LEN+1] = {0,};
    char ip_dst[IP_LEN+1] = {0,};
    char ip_src_two[IP_LEN/4+1] = {0,}; // ip_src最后两位
    char ip_dst_two[IP_LEN/4+1] = {0,}; // ip_dst最后两位

    int matrix[MAX_NUM][MAX_NUM];
    memset(matrix, 0x3f, sizeof(matrix)); // 记录拓扑
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

    // 读取拓扑
    printf("start to read topo\n");
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
            matrix[node1][node2] = delay;
        }
    }
    for(i = 0; i < MAX_NUM; i++)
    {
        matrix[i][i] = 0;
    }
    freeReplyObject(reply);
    redisFree(context);
    printf("finish to read topo\n");

    // 去掉下个时间片要删除的链路
    printf("start to revise topo(del some links)\n");
    snprintf(cmd, CMD_MAX_LENGHT, "smembers del_link_%02d", slot);
    context = redisConnect(redis_ip, REDIS_SERVER_PORT);
    if (context->err)
    {
        printf("Error: %s\n", context->errstr);
        redisFree(context);
        return NULL;
    }
    printf("connect redis server success\n");

    reply = (redisReply *)redisCommand(context, cmd);
    if (reply != NULL && reply->elements != 0)
    {
        printf("del_link num = %lu\n",reply->elements);
        for(i = 0; i < reply->elements; i++)
        {
            sw = atol(reply->element[i]->str);
            sw1 = (uint32_t)((sw & 0xffffffff00000000) >> 32);
            sw2 = (uint32_t)(sw & 0x00000000ffffffff);
            printf("del_link: sw%02d<->sw%02d\n", sw1, sw2);
            matrix[sw1][sw2] = MAX_DIST;
        }
    }
    freeReplyObject(reply);
    redisFree(context);

    // Floyd 计算任意两点间距离
    printf("start to run Floyd\n");
    for(k = 0; k < MAX_NUM; k++)
    {//从0开始遍历每一个中间节点，代表允许经过的结点编号<=k 
        for(i = 0; i < MAX_NUM; i++)
        {
            for(j = 0; j < MAX_NUM; j++)
            {
                if(matrix[i][k] == MAX_DIST || matrix[k][j] == MAX_DIST) 
                    continue;//中间节点不可达 
                if(matrix[i][j] == MAX_DIST || matrix[i][k] + matrix[k][j] < matrix[i][j])//经过中间节点，路径变短 
                {
                    matrix[i][j] = matrix[i][k] + matrix[k][j];
                    route[i][j] = k;
                }
            }
        }
    }
    printf("finish to run Floyd\n");

    /*组装Redis命令*/
    snprintf(cmd, CMD_MAX_LENGHT, "smembers del_link_%02d", slot);

    /*连接redis*/
    context1 = redisConnect(redis_ip, REDIS_SERVER_PORT);
    if (context1->err)
    { 
        printf("Error: %s\n", context1->errstr);
        redisFree(context1);
        return NULL;
    }
    printf("connect redis server success\n");

    /*执行redis命令*/
    reply1 = (redisReply *)redisCommand(context1, cmd);
    if (reply1 != NULL && reply1->elements != 0)
    {
        // 输出查询结果
        printf("del_link num = %lu\n",reply1->elements);

        for(i = 0; i < reply1->elements; i++)
        {
            sw = atol(reply1->element[i]->str);
            sw1 = (uint32_t)((sw & 0xffffffff00000000) >> 32);
            sw2 = (uint32_t)(sw & 0x00000000ffffffff);
            printf("del_link: sw%02d<->sw%02d\n", sw1, sw2);

            ctrl_id = sw1;
            db_id = Get_Ctrl_Conn_Db((uint32_t)ctrl_id, redis_ip);

            // 查询相关的非定时路由
            /*组装Redis命令*/
            snprintf(cmd, CMD_MAX_LENGHT, "smembers rt_set_%02d_%02d", sw1, sw2);

            /*连接redis*/
            context2 = redisConnect(redis_ip, REDIS_SERVER_PORT);
            if (context2->err)
            { 
                printf("Error: %s\n", context2->errstr);
                redisFree(context2);
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
            printf("route num = %lu\n",reply2->elements);
            if(reply2->elements == 0)
            {
                freeReplyObject(reply2);
                redisFree(context2);
                continue;
            }
            for(k = 0; k < reply2->elements; k++)
            {
                printf("route entry: %s\n",reply2->element[k]->str);
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
                    Del_Rt_Set(slot, ip_src, ip_dst, redis_ip);

                    // 判断是正在工作的c2d控制通道路由
                    if(((strstr(ip_dst, redis_ip) != NULL) && (strstr(ip_src, "44") == NULL)) || ((strstr(ip_src, redis_ip) != NULL) && (strstr(ip_dst, "44") == NULL)))
                    {
                        // 优雅关闭tcp套接字，通知控制器切换数据库
                        shutdown(fd[ctrl_id], SHUT_WR);
                        fd_close[fd_close_num++] = fd[ctrl_id];
                        fd[ctrl_id] = 0;
                    }

                    // 向数据库写入新路由
                    strncpy(ip_dst_two, reply2->element[k]->str+IP_LEN+6, 2);
                    sw1 = strtoi(ip_src_two, 2) - 1;
                    sw2 = strtoi(ip_dst_two, 2) - 1;
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
                        Set_Cal_Route(ip_src, ip_dst, out_sw_port, redis_ip);
                        memset(out_sw_port, 0, CMD_MAX_LENGHT);
                    }
                }
            }
            freeReplyObject(reply2);
            redisFree(context2);
        }

        sleep(2);
        for(i = 0; i < fd_close_num; i++)
        {
            close(fd_close[i]);
        }
    }
  
    freeReplyObject(reply1);
    redisFree(context1);
    return NULL;
}

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
    
    if(listen_init(redis_ip) != 0)
    {
        printf("套接字初始化失败\n"); 
        return NULL;
    }
    while(1)
    {
        bzero(&buf, sizeof(buf));
        recvfrom(server_fd, buf, BUFSIZE, 0, (struct sockaddr*)clent_addr, &len);
        slot = atoi(buf);
        memset(fail_link_index, 0 , sizeof(fail_link_index));

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

    char ip_src[IP_LEN+1] = {0,};
    char ip_dst[IP_LEN+1] = {0,};
    int timeout = 0;
    strncpy(ip_src, &obj[6], IP_LEN);
    strncpy(ip_dst, &obj[6 + IP_LEN], IP_LEN);

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
            cfd = fd[ctrl_id]; 
            // printf("cfd:%ld\n", cfd);

            // 不设置定时
            timeout = 0;
            // add route to routes set <-> link
            Add_Rt_Set((uint32_t)sw, (uint32_t)port, ip_src, ip_dst, redis_ip);

            // type:1,sw:3,ip_src:8,ip_dst:8,outport:3,timeout:3
            memset(buf, 0, BUFSIZE);
            printf("db_id:%d, ctrl_id:%d, sw:%d, ip_src:%s, ip_dst:%s, port:%d, timeout:%d\n", db_id, ctrl_id, sw, ip_src, ip_dst, port, timeout);
            snprintf(buf, BUFSIZE, "%d%03d%s%s%03d%03d", ROUTE_ADD, sw, ip_src, ip_dst, port, timeout);
            printf("buf:%s\n",buf);
            ret = send(cfd, buf, BUFSIZE, 0);
            if (ret == -1)
            {
                print_err("send route failed", __LINE__, errno);
                // 发送失败表示控制器断开连接，将 buf 存入对应的待执行结构 wait_exec_X
                Add_Wait_Exec(ctrl_id, buf, redis_ip);
            }
        }
    }

    freeReplyObject(reply);
    redisFree(context);
    return 0;
}

// 遍历传入的失效链路，将失效链路上的全部路由都调整为可行的新路由，向相应的控制器发送 删除/新增 流表项通告
// 计算新路由时采用的真实拓扑real_topo，可能会在新路由下发过程中发生变化，为了保证路由调整的有效性，控制器在下发新增流表前需要判断其合法性
int route_del(char *obj, int index, char *redis_ip)
{
    char cmd[CMD_MAX_LENGHT] = {0};
    redisContext *context;
    redisReply *reply;
    int i, j, k = 0;
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
    char ip_src_two[IP_LEN/4+1] = {0,}; // ip_src最后两位
    char ip_dst_two[IP_LEN/4+1] = {0,}; // ip_dst最后两位

    // char slot_str[2] = {0,};
    // strncpy(slot_str, obj+10, 2);
    // int slot = atoi(slot_str);

    int matrix[MAX_NUM][MAX_NUM];
    memset(matrix, 0x3f, sizeof(matrix)); // 记录拓扑
    uint32_t node1, node2 = 0;
    uint64_t delay = 0;
    int route[MAX_NUM][MAX_NUM]; // 记录松弛节点
    memset(route, -1, sizeof(route));
    int nextsw[MAX_NUM]; // 记录下一跳交换机节点
    int hop = 0;
    char out_sw_port[CMD_MAX_LENGHT] = {0,}; // 存储出端口列表
    char sw_port[8] = {0,}; // 存储出端口

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
        freeReplyObject(reply);
        redisFree(context);
        return -1;
    }
    sw = atol(reply->str);
    fail_sw1 = (uint32_t)((sw & 0xffffffff00000000) >> 32);
    fail_sw2 = (uint32_t)(sw & 0x00000000ffffffff); // 失效节点
    printf("fail_link: sw%d<->sw%d\n", fail_sw1, fail_sw2); 
    freeReplyObject(reply);
    redisFree(context);

    // 读取拓扑
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
                matrix[node1][node2] = delay;
        }
    }
    for(i = 0; i < MAX_NUM; i++)
    {
        matrix[i][i] = 0;
    }
    freeReplyObject(reply);
    redisFree(context);

    // 去掉下个时间片要删除的链路
    printf("start to revise topo(del some links)\n");
    snprintf(cmd, CMD_MAX_LENGHT, "smembers del_link_%02d", slot);
    context = redisConnect(redis_ip, REDIS_SERVER_PORT);
    if (context->err)
    {
        printf("Error: %s\n", context->errstr);
        redisFree(context);
        return FAILURE;
    }
    printf("connect redis server success\n");

    reply = (redisReply *)redisCommand(context, cmd);
    if (reply != NULL && reply->elements != 0)
    {
        printf("del_link num = %lu\n",reply->elements);

        for(i = 0; i < reply->elements; i++)
        {
            sw = atol(reply->element[i]->str);
            sw1 = (uint32_t)((sw & 0xffffffff00000000) >> 32);
            sw2 = (uint32_t)(sw & 0x00000000ffffffff);
            printf("del_link: sw%02d<->sw%02d\n", sw1, sw2);
            matrix[sw1][sw2] = MAX_DIST;
        }
    }
    freeReplyObject(reply);
    redisFree(context);

    // Floyd 计算任意两点间距离
    printf("start to run Floyd\n");
    for(k = 0; k < MAX_NUM; k++)
    {//从0开始遍历每一个中间节点，代表允许经过的结点编号<=k 
        for(i = 0; i < MAX_NUM; i++)
        {
            for(j = 0; j < MAX_NUM; j++)
            {
                if(matrix[i][k] == MAX_DIST || matrix[k][j] == MAX_DIST) 
                    continue;//中间节点不可达 
                if(matrix[i][j] == MAX_DIST || matrix[i][k] + matrix[k][j] < matrix[i][j])//经过中间节点，路径变短 
                {
                    matrix[i][j] = matrix[i][k] + matrix[k][j];
                    route[i][j] = k;
                }
            }
        }
    }
    printf("finish to run Floyd\n");

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
    printf("entry num = %lu\n",reply->elements);
    if(reply->elements == 0) 
    {
        freeReplyObject(reply);
        redisFree(context);
        return -1;
    }
    for(i = 0; i < reply->elements; i++)
    {
        printf("route entry: %s\n",reply->element[i]->str);
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
            
            // 判断是正在工作的控制通道路由
            if(strstr(ip_dst, redis_ip) != NULL)
            {
                // 关闭tcp套接字，通知控制器切换数据库
                close(fd[ctrl_id]);
            }
            
            cfd = fd[ctrl_id];
            // printf("cfd:%ld\n", cfd);
            // type:1,sw:3,ip_src:8,ip_dst:8,outport:3,timeout:3
            memset(buf, 0, BUFSIZE);
            snprintf(buf, BUFSIZE, "%d%03ld%s%03d%03d", ROUTE_DEL, sw, reply->element[i]->str, 0, 0);
            ret = send(cfd, buf, BUFSIZE, 0);
            if (ret == -1)
            {
                print_err("send route failed", __LINE__, errno);
                // 发送失败表示控制器断开连接，将 buf 存入对应的待执行结构 wait_exec_X
                Add_Wait_Exec(ctrl_id, buf, redis_ip);
            }
            if(strstr(reply->element[2]->str, "rpush") != NULL)

            // 删除该路由的链路映射
            Del_Rt_Set(slot, ip_src, ip_dst, redis_ip);

            // 向数据库写入新路由
            strncpy(ip_dst_two, reply->element[i]->str+IP_LEN+6, 2);
            sw1 = strtoi(ip_src_two, 2) - 1;
            sw2 = strtoi(ip_dst_two, 2) - 1;
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
                Set_Cal_Route(ip_src, ip_dst, out_sw_port, redis_ip);
                memset(out_sw_port, 0, CMD_MAX_LENGHT);
            }
        }
    }

    freeReplyObject(reply);
    redisFree(context);
    return 0;
}

// 订阅回调函数
void psubCallback(redisAsyncContext *c, void *r, void *redis_ip) 
{
    int i = 0;
    redisReply *reply = (redisReply*)r;
    if (reply == NULL) return;
    char reply_str[26] = {0,};
    char ip_two[IP_LEN/4+1] = {0,}; // redis_ip最后两位
    int redis_id = -1;

    int ctrl_id = 0;  // 记录控制器ID
    char ctrl_str[3] = {0,};
    int db_id = 0;
    long cfd = -1;
    int ret = -1;
    char buf[BUFSIZE] = {0,};
    char cmd[CMD_MAX_LENGHT] = {0};
    redisContext *context1;
    redisReply *reply1;

    // 订阅接收到的消息是一个带三元素的数组
    if (reply->type == REDIS_REPLY_ARRAY && reply->elements == 3) 
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
    if (reply->type == REDIS_REPLY_ARRAY && reply->elements == 4)
    {
        printf("Recieved message:\n\t(1)channel: %s\n\t(2)option: %s\n\t(3)object: %s\n", 
                reply->element[1]->str,
                reply->element[2]->str,
                reply->element[3]->str);

        // 判断操作是否为rpush
        if(strstr(reply->element[2]->str, "rpush") != NULL)
        {
            if(strstr(reply->element[3]->str, "calrt") != NULL)
            {
                // 查询数据库，下发流表项
                if(route_add(reply->element[3]->str, redis_ip) == -1)
                    printf("cal route add failure\n");
                else
                    printf("cal route add success\n");
            }
            // 判断是fail_link
            else if(strstr(reply->element[3]->str, "fail_link") != NULL) 
            {
                // 查询数据库，下发流表项
                strncpy(ip_two, reply->element[3]->str+10, 2);
                redis_id = atoi(ip_two);
                if(route_del(reply->element[3]->str, fail_link_index[redis_id], redis_ip) == -1)
                    printf("route del failure\n");
                else
                    printf("route del success\n");
                fail_link_index[redis_id]++;
            }
        }
        // 判断操作是否为sadd
        else if(strstr(reply->element[2]->str, "sadd") != NULL)
        {
            if(strstr(reply->element[3]->str, "wait_exec") != NULL)
            {
                strncpy(ctrl_str, reply->element[3]->str+10, 2);
                ctrl_id = atol(ctrl_str);
                db_id = Get_Ctrl_Conn_Db((uint32_t)ctrl_id, redis_ip);

                // 判断起点属于本区域交换机，向对应控制器发送通告
                // DB_ID = (((inet_addr(redis_ip))&0xff000000)>>24) - 1
                if(db_id == (((inet_addr(redis_ip))&0xff000000)>>24) - 1)
                {
                    cfd = fd[ctrl_id];

                    // 查询数据库，下发流表项
                    snprintf(cmd, CMD_MAX_LENGHT, "smembers %s", reply->element[3]->str);
                    context1 = redisConnect(redis_ip, REDIS_SERVER_PORT);
                    if (context1->err)
                    {
                        printf("Error: %s\n", context1->errstr);
                        redisFree(context1);
                        return;
                    }
                    printf("connect redis server success\n");
                    reply1 = (redisReply *)redisCommand(context1, cmd);
                    if (reply1 == NULL)
                    {
                        printf("execute command:%s failure\n", cmd);
                        redisFree(context1);
                        return;
                    }

                    // 输出查询结果
                    printf("entry num = %lu\n",reply1->elements);
                    if(reply1->elements == 0) 
                    {
                        freeReplyObject(reply1);
                        redisFree(context1);
                        return;
                    }
                    for(i = 0; i < reply1->elements; i++)
                    {
                        printf("ctrl_%d buf_%d: %s\n", ctrl_id, i, reply1->element[i]->str);
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
    
	skfd = socket(AF_INET, SOCK_STREAM, 0);
	if (skfd == -1) 
    {
		print_err("socket failed",__LINE__,errno);
	}

	struct sockaddr_in addr;
	addr.sin_family = AF_INET; // 设置tcp协议族
	addr.sin_port = htons(SERVER_PORT); // 设置端口号
	addr.sin_addr.s_addr = inet_addr(redis_ip); // 设置ip地址

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

    return 0;
}
