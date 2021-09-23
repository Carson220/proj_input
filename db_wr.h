/***************************************************************
*   文件名称：db_wr.h
*   描    述：用于向Redis数据库进行读写操作

1. controller读取当前时间片的topo
    接口input：uint32_t slot_no, tp_sw sw_list[SW_NUM]
    其中，sw_list为tp_sw类型，对每一条边调用函数(在mulhello的topo.c中)
    int tp_add_link(uint32_t sw_dpid, uint32_t port1, uint32_t sw_dpid_adj, uint32_t port2, uint64_t delay, tp_sw sw_list[SW_NUM])

2. 维护controller当前连接的sw集合，db代理分发路由时需要查询结果，发送给相应的控制器
    接口input：int ctrl_id, uint32_t sw_dpid
    可采用set数据结构存储，https://blog.csdn.net/Xiejingfa/article/details/50594005

3. 控制器确认链路连接之后，写入数据库的当前时间片真实拓扑real_topo
    接口input：uint32_t sw_dpid, uint32_t port1, uint32_t sw_dpid_adj, uint32_t port2

4. 控制器发现链路断开之后，需要从real_topo中删除，并加入到失效链路集合fail_link中
    接口input：uint32_t sw_dpid, uint32_t port1, uint32_t sw_dpid_adj, uint32_t port2

5. 控制器将计算好的路由写入数据库（一次完成整个条目的写入）
    接口input：uint32_t nw_src, uint32_t nw_dst, uint32_t sw_dpid, uint32_t outport
    
***************************************************************/

#include "hiredis.h"
#include "topo.h"

/*宏定义*/
#define CMD_MAX_LENGHT 256
// #define REDIS_SERVER_IP "192.168.10.215"
#define REDIS_SERVER_PORT 8102

typedef enum DB_RESULT
{
    SUCCESS = 1,
    FAILURE = -1
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
// write controller <-> switches set
DB_RESULT Add_Sw_Set(uint32_t ctrl, uint32_t sw, int slot, char *redis_ip);
DB_RESULT Del_Sw_Set(uint32_t ctrl, uint32_t sw, int slot, char *redis_ip);
// write controller <-> database
DB_RESULT Set_Ctrl_Conn_Db(uint32_t ctrl, uint32_t db, int slot, char *redis_ip);
// write default topo
DB_RESULT Set_Topo(uint32_t port1, uint32_t port2, uint64_t delay, int slot, char *redis_ip);
// write real topo that links must be connected
DB_RESULT Add_Real_Topo(uint32_t port1, uint32_t port2, int slot, char *redis_ip);
DB_RESULT Del_Real_Topo(uint32_t port1, uint32_t port2, int slot, char *redis_ip);
// write default routes(s2s/d2d/c2s/c2d)
DB_RESULT Set_Dfl_Route(char *ip_src, char *ip_dst, char *out_sw_port, int slot, char *redis_ip);
DB_RESULT Set_Cal_Route(char *ip_src, char *ip_dst, char *out_sw_port, int slot, char *redis_ip);
// write links that next slot will be deleted
DB_RESULT Set_Del_Link(uint32_t sw1, uint32_t sw2, int slot, char *redis_ip);
// write links that have been disconnected
DB_RESULT Set_Fail_Link(uint32_t sw1, uint32_t sw2, int slot, char *redis_ip);

/*读函数*/
// uint16_t Get_Ctrl_Id(uint32_t ip);                       /*获取控制器ID*/
// uint64_t Get_Link_Delay(uint32_t port1, uint32_t port2); /*获取链路时延*/
// uint32_t Get_Pc_Sw_Port(uint32_t ip);                    /*获取PC连接的交换机端口*/
// uint64_t Get_Sw_Delay(uint16_t cid, uint8_t sid);        /*获取交换机到控制器的时延*/

// read switch <-> controller(active and standby)
uint32_t Get_Active_Ctrl(uint32_t sw, int slot, char *redis_ip);
uint32_t Get_Standby_Ctrl(uint32_t sw, int slot, char *redis_ip);
// lookup controller <-> switches set
DB_RESULT Lookup_Sw_Set(uint32_t ctrl, uint32_t sw, int slot, char *redis_ip);
// read controller <-> database
uint32_t Get_Ctrl_Conn_Db(uint32_t ctrl, int slot, char *redis_ip);
//read default topo from redis to sw_list
DB_RESULT Get_Topo(int slot, char *redis_ip, tp_sw sw_list[SW_NUM]);
//read real topo from redis to sw_list
DB_RESULT Get_Real_Topo(int slot, char *redis_ip, tp_sw sw_list[SW_NUM]);
// read link delay
uint64_t Get_Link_Delay(uint32_t port1, uint32_t port2, int slot, char *redis_ip);

/*执行命令*/
DB_RESULT redis_connect(redisContext **context, char *redis_ip);
DB_RESULT exeRedisIntCmd(char *cmd, char *redis_ip); // 写操作返回int
