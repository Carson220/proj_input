/***************************************************************
*   文件名称：db_wr.h
*   描    述：用于向Redis数据库进行读写操作 
***************************************************************/

#include "hiredis.h"

/*宏定义*/
#define CMD_MAX_LENGHT 256
// #define REDIS_SERVER_IP "192.168.10.215"
#define REDIS_SERVER_PORT 8102

typedef enum DB_RESULT
{
    SUCCESS = 0,
    FAILURE = 1
} DB_RESULT;

/*写函数*/
// DB_RESULT Set_Ctrl_Id(uint32_t ip, uint16_t id);                         /*设置控制器信息 IP->ID*/
// DB_RESULT Set_Link_Delay(uint32_t port1, uint32_t port2, uint64_t delay);   /*设置链路信息 (node1,node2)->时延*/
// DB_RESULT Clr_Link_Delay(uint32_t port1, uint32_t port2);                   /*清除链路信息*/
// DB_RESULT Set_Pc_Sw_Port(uint32_t ip, uint32_t port);                       /*设置PC信息 IP->连接的交换机端口*/
// DB_RESULT Set_Sw_Delay(uint16_t cid, uint8_t sid, uint64_t delay);          /*设置交换机信息 (CID,SID)->到控制器的时延*/
// DB_RESULT Clr_Sw_Delay(uint16_t cid, uint8_t sid);                          /*清除交换机信息*/
// DB_RESULT Set_Route(uint32_t ip_src, uint32_t ip_dst, uint32_t out_sw_port);/*设置路由信息 添加到列表头部*/
// DB_RESULT Clr_Route(uint32_t ip_src, uint32_t ip_dst);                      /*清除路由信息*/

// write switch <-> controller(active and standby)
DB_RESULT Set_Active_Ctrl(uint32_t sw, uint32_t ctrl, int slot, char *redis_ip);
DB_RESULT Set_Standby_Ctrl(uint32_t sw, uint32_t ctrl, int slot, char *redis_ip);
// write controller <-> database
DB_RESULT Set_Ctrl_Conn_Db(uint32_t ctrl, uint32_t db, int slot, char *redis_ip);
// write topo
DB_RESULT Set_Topo(uint32_t port1, uint32_t port2, uint64_t delay, int slot, char *redis_ip);
// write default routes(s2s/d2d/c2s/c2d)
DB_RESULT Set_Dfl_Route(char *ip_src, char *ip_dst, char *out_sw_port, int slot, char *redis_ip);
// write links that next slot will be deleted
DB_RESULT Set_Del_Link(uint32_t sw1, uint32_t sw2, int slot, char *redis_ip);

/*读函数*/
// uint16_t Get_Ctrl_Id(uint32_t ip);                       /*获取控制器ID*/
// uint64_t Get_Link_Delay(uint32_t port1, uint32_t port2); /*获取链路时延*/
// uint32_t Get_Pc_Sw_Port(uint32_t ip);                    /*获取PC连接的交换机端口*/
// uint64_t Get_Sw_Delay(uint16_t cid, uint8_t sid);        /*获取交换机到控制器的时延*/

// read switch <-> controller(active and standby)
uint32_t Get_Active_Ctrl(uint32_t sw, int slot, char *redis_ip);
uint32_t Get_Standby_Ctrl(uint32_t sw, int slot, char *redis_ip);
// read controller <-> database
uint32_t Get_Ctrl_Conn_Db(uint32_t ctrl, int slot, char *redis_ip);

/*执行命令*/
DB_RESULT redis_connect(redisContext **context, char **redis_ip);
DB_RESULT exeRedisIntCmd(char *cmd, char *redis_ip); // 写操作返回int
