// init link-route map

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "db_wr.h"
// #include "db_wr.c"

#define slot_num 44
#define db_num 6
#define sw_num 66
#define addr_len 8 + 1
#define redis_ip_len 20

int main(int argc,char *argv[])
{
    uint32_t sw1, sw2;
    int i, j, k, m = 0;
    int ret = -1;
    int db_id[db_num] = {13, 16, 31, 46, 50, 54};

    char ip_src[addr_len]  = {0,};
    char ip_dst[addr_len]  = {0,};
    // char redis_ip[redis_ip_len] = {0,};
    int slot = atol(argv[1]);
    char *redis_ip = argv[2];

    char cmd[CMD_MAX_LENGHT] = {0};
    redisContext *context=NULL;
    redisReply *reply=NULL;

    for(i = 0; i < db_num; i++)
    {
        // d2d
        for(j = 0; j < db_num; j++)
        {
            if(i != j)
            {
                snprintf(ip_src, addr_len, "c0a844%02x", db_id[i]+1); // 192.168.68.X
                snprintf(ip_dst, addr_len, "c0a844%02x", db_id[j]+1);

                // lookup route
                snprintf(cmd, CMD_MAX_LENGHT, "lrange dflrt_%s%s_%02d 0 -1", ip_src, ip_dst, slot);
                if(context == NULL)
                {
                    do{
                        context = NULL;
                        ret = redis_connect(&context, redis_ip); 
                        usleep(3000);
                    }while(ret == FAILURE);
                }
                reply = (redisReply *)redisCommand(context, cmd);
                if (reply == NULL)
                {
                    printf("\texecute command:%s failure\n", cmd);
                    redisFree(context);
                    return -1;
                }

                // 输出查询结果
                printf("dflrt_%s%s_%02d entry num = %lu\n", ip_src, ip_dst, slot, reply->elements);
                if(reply->elements == 0) return -1;
                for(m = 0; m < reply->elements; m++)
                {
                    printf("\tout_sw_port: %s\n",reply->element[m]->str);
                    sw1 = atoi(reply->element[m]->str)/1000;
                    sw2 = atoi(reply->element[m]->str)%1000;
                    // add link-route map
                    Add_Rt_Set(sw1, sw2, ip_src, ip_dst, redis_ip);
                }
                
                freeReplyObject(reply);
            }
        }

        // d2c c2d
        for(k = 0; k < sw_num; k++)
        {
            if(db_id[i] != k)
            {
                snprintf(ip_src, addr_len, "c0a844%02x", db_id[i]+1); // 192.168.68.X
                snprintf(ip_dst, addr_len, "c0a842%02x", k+1); // 192.168.66.X

                // lookup route
                snprintf(cmd, CMD_MAX_LENGHT, "lrange dflrt_%s%s_%02d 0 -1", ip_src, ip_dst, slot);
                context = redisConnect(redis_ip, REDIS_SERVER_PORT);
                if (context->err)
                {
                    redisFree(context);
                    printf("\tError: %s\n", context->errstr);
                    return -1;
                }
                // printf("\tconnect redis server success\n");
                reply = (redisReply *)redisCommand(context, cmd);
                if (reply == NULL)
                {
                    printf("\texecute command:%s failure\n", cmd);
                    redisFree(context);
                    return -1;
                }

                // 输出查询结果
                printf("dflrt_%s%s_%02d  entry num = %lu\n", ip_src, ip_dst, slot, reply->elements);
                if(reply->elements == 0) return -1;
                for(m = 0; m < reply->elements; m++)
                {
                    printf("\tout_sw_port: %s\n",reply->element[m]->str);
                    sw1 = atoi(reply->element[m]->str)/1000;
                    sw2 = atoi(reply->element[m]->str)%1000;
                    // add link-route map
                    Add_Rt_Set(sw1, sw2, ip_src, ip_dst, redis_ip);
                    Add_Rt_Set(sw2, sw1, ip_dst, ip_src, redis_ip);
                }
                
                freeReplyObject(reply);
            }
        }
    }

    redisFree(context);
    return 0;
}