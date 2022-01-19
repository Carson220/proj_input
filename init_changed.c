// init database content

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "db_wr.h"
#include <unistd.h>
// #include "db_wr.c"

#define slot_num 44
#define db_num 6
#define fname_len 50
#define addr_len 10
#define redis_ip_len 20
#define DB_ID 1 // database_id = 192.168.68.2

RET_RESULT wr_topo(int slot_no, FILE *fp, char* redis_ip)
{
    int num = 0, ret;
    uint32_t sw, sw1, sw2;
    uint64_t delay;
    float dist;
    redisContext *context = NULL;
    redisReply *reply = NULL;
    char cmd[CMD_MAX_LENGHT] = {0};
    uint64_t sw_tmp;

    fscanf(fp, "%d", &num);
    /*连接redis*/
    while(ret == FAILURE)
    {
        context = NULL;
        ret = redis_connect(&context, redis_ip); 
        usleep(3000);
    }

    while(!feof(fp))
    {
        fscanf(fp, "%d %d %f", &sw1, &sw2, &dist);
        delay = (int)(dist*1e5);
        sw_tmp = (((uint64_t)sw1) << 32) + sw2;
        /*组装redis命令*/
        snprintf(cmd, CMD_MAX_LENGHT, "hset dfl_topo_%02d %lu %lu", slot_no, sw_tmp, delay);
        /*执行redis命令*/
        if(context == NULL)
        {
            do{
                context = NULL;
                ret = redis_connect(&context, redis_ip); 
                usleep(3000);
            }while(ret == FAILURE);
        }
        reply = (redisReply *)redisCommand(context, cmd);
        if (NULL == reply)
        {
            printf("%d execute command:%s failure\n", __LINE__, cmd);
            redisFree(context);
            return FAILURE;
        }
        freeReplyObject(reply);
        printf("%d execute command:%s success\n", __LINE__, cmd);
        // Set_Topo(sw1, sw2, delay, slot_no, (char*)redis_ip);

        // /*组装redis命令*/
        // snprintf(cmd, CMD_MAX_LENGHT, "sadd dfl_set_%02d %lu", slot_no, sw_tmp);
        // /*执行redis命令*/
        // if(context == NULL)
        // {
        //     do{
        //         context = NULL;
        //         ret = redis_connect(&context, redis_ip); 
        //         usleep(3000);
        //     }while(ret == FAILURE);
        // }
        // reply = (redisReply *)redisCommand(context, cmd);
        // if (NULL == reply)
        // {
        //     printf("%d execute command:%s failure\n", __LINE__, cmd);
        //     redisFree(context);
        //     return FAILURE;
        // }
        // freeReplyObject(reply);
        // printf("%d execute command:%s success\n", __LINE__, cmd);
    }
    fclose(fp);
    redisFree(context);
    return SUCCESS;
}

// RET_RESULT wr_sw2ctrl_active_standby(int slot_no, FILE *fp1, FILE *fp2, char* redis_ip)
// {
//     int num = 0, ret;
//     uint32_t sw, ctrl1, ctrl2;
//     redisContext *context = NULL;
//     redisReply *reply = NULL;
//     char cmd[CMD_MAX_LENGHT] = {0};
//     uint64_t sw_tmp;

//     while(ret == FAILURE)
//     {
//         context = NULL;
//         ret = redis_connect(&context, redis_ip); 
//         usleep(3000);
//     }
//     fscanf(fp1, "%d\n", &num);
//     fscanf(fp2, "%d\n", &num);

//     for(sw = 0; sw < num; sw++)
//     {
//         fscanf(fp1, "%d", &ctrl1);
//         fscanf(fp2, "%d", &ctrl2);

//         // Set_Active_Ctrl(sw, ctrl1, slot_no, redis_ip);
//         snprintf(cmd, CMD_MAX_LENGHT, "hset active_ctrl_%02d %u %u", slot_no, sw, ctrl1);
//         /*执行redis命令*/
//         if(context == NULL)
//         {
//             do{
//                 context = NULL;
//                 ret = redis_connect(&context, redis_ip); 
//                 usleep(3000);
//             }while(ret == FAILURE);
//         }
//         reply = (redisReply *)redisCommand(context, cmd);
//         if (NULL == reply)
//         {
//             printf("%d execute command:%s failure\n", __LINE__, cmd);
//             redisFree(context);
//             return FAILURE;
//         }
//         freeReplyObject(reply);
//         printf("%d execute command:%s success\n", __LINE__, cmd);

//         // Set_Standby_Ctrl(sw, ctrl2, slot_no, redis_ip);
//         snprintf(cmd, CMD_MAX_LENGHT, "hset standby_ctrl_%02d %u %u", slot_no, sw, ctrl2);
//         /*执行redis命令*/
//         if(context == NULL)
//         {
//             do{
//                 context = NULL;
//                 ret = redis_connect(&context, redis_ip); 
//                 usleep(3000);
//             }while(ret == FAILURE);
//         }
//         reply = (redisReply *)redisCommand(context, cmd);
//         if (NULL == reply)
//         {
//             printf("%d execute command:%s failure\n", __LINE__, cmd);
//             redisFree(context);
//             return FAILURE;
//         }
//         freeReplyObject(reply);
//         printf("%d execute command:%s success\n", __LINE__, cmd);
//     }

//     fclose(fp1);
//     fclose(fp2);
//     redisFree(context);
//     return SUCCESS;
// }

// RET_RESULT wr_Ctrl_Conn_Db(int slot_no, FILE *fp, char* redis_ip)
// {
//     int j, db, ctrl, ret;
//     char cmd[CMD_MAX_LENGHT] = {0};
//     redisContext *context = NULL;
//     redisReply *reply = NULL;

//     while(ret == FAILURE)
//     {
//         context = NULL;
//         ret = redis_connect(&context, redis_ip); 
//         usleep(3000);
//     }
//     for(j = 0; j < db_num; j++)
//     {
//         fscanf(fp, "%d", &db);
//         fgetc(fp); // read '\n'
//         while(fgetc(fp) == ' ')
//         {
//             fscanf(fp, "%d", &ctrl);
//             // Set_Active_Ctrl(ctrl, db, slot_no, redis_ip);
//             snprintf(cmd, CMD_MAX_LENGHT, "hset db_%02d %u %u", slot_no, ctrl, db);
//             if(context == NULL)
//             {
//                 do{
//                     context = NULL;
//                     ret = redis_connect(&context, redis_ip); 
//                     usleep(3000);
//                 }while(ret == FAILURE);
//             }
//             reply = (redisReply *)redisCommand(context, cmd);
//             if (NULL == reply)
//             {
//                 printf("%d execute command:%s failure\n", __LINE__, cmd);
//                 redisFree(context);
//                 return FAILURE;
//             }
//             freeReplyObject(reply);
//             printf("%d execute command:%s success\n", __LINE__, cmd);
//         }
//     }

//     fclose(fp);
//     redisFree(context);
//     return SUCCESS;
// }

// RET_RESULT wr_dfl_s2s(int slot_no, FILE *fp, char* redis_ip)
// {
//     int j, db, ctrl, ret, num=66, sw1, sw2, k;
//     char cmd[CMD_MAX_LENGHT] = {0};
//     char out_sw_port[CMD_MAX_LENGHT] = {0};
//     char ip_src[addr_len]  = {0};
//     char ip_dst[addr_len]  = {0};
//     redisContext *context = NULL;
//     redisReply *reply = NULL;

//     while(ret == FAILURE)
//     {
//         context = NULL;
//         ret = redis_connect(&context, redis_ip); 
//         usleep(3000);
//     }
//     for(j = 0; j < num; j++)
//     {
//         fscanf(fp, "%d", &sw1);
//         for(k = 1; k < num; k++)
//         {
//             fscanf(fp, "%d", &sw1);
//             fscanf(fp, "%d", &sw2);
//             fgets(out_sw_port, CMD_MAX_LENGHT, fp);
            
//             snprintf(ip_src, addr_len, "c0a842%02x", sw1+1); // 192.168.66.X
//             snprintf(ip_dst, addr_len, "c0a842%02x", sw2+1);

//             // Set_Dfl_Route(ip_src, ip_dst, out_sw_port, slot_no, redis_ip);
//             snprintf(cmd, CMD_MAX_LENGHT, "rpush dflrt_%s%s_%02d %s", ip_src, ip_dst, slot_no, out_sw_port);
//             if(context == NULL)
//             {
//                 do{
//                     context = NULL;
//                     ret = redis_connect(&context, redis_ip); 
//                     usleep(3000);
//                 }while(ret == FAILURE);
//             }
//             reply = (redisReply *)redisCommand(context, cmd);
//             if (NULL == reply)
//             {
//                 printf("%d execute command:%s failure\n", __LINE__, cmd);
//                 redisFree(context);
//                 return FAILURE;
//             }
//             freeReplyObject(reply);
//             printf("%d execute command:%s success\n", __LINE__, cmd);
//         }
//     }
//     fclose(fp);
//     redisFree(context);
//     return SUCCESS;
// }

RET_RESULT wr_dfl_d2d(int slot_no, FILE *fp, char* redis_ip)
{
    int j, db, ctrl, ret, num=66, sw1, sw2, k, sw;
    char cmd[CMD_MAX_LENGHT] = {0};
    char out_sw_port[CMD_MAX_LENGHT] = {0}; // 存储出端口列表
    char sw_port[8] = {0,}; // 存储出端口
    int hop = 0;
    char ip_src[addr_len]  = {0};
    char ip_dst[addr_len]  = {0};
    redisContext *context = NULL;
    redisReply *reply = NULL;

    while(ret == FAILURE)
    {
        context = NULL;
        ret = redis_connect(&context, redis_ip); 
        usleep(3000);
    }
    for(j = 0; j < db_num; j++)
    {
        fscanf(fp, "%d", &sw1);
        for(k = 1; k < db_num; k++)
        {
            // d2d_1
            fscanf(fp, "%d", &sw1);
            fscanf(fp, "%d", &sw2);
            fgets(out_sw_port, CMD_MAX_LENGHT, fp);
            out_sw_port[strlen(out_sw_port)-1]='\0';
            
            snprintf(ip_src, addr_len, "c0a844%02x", sw1+1); // 192.168.68.X
            snprintf(ip_dst, addr_len, "c0a844%02x", sw2+1);

            // Set_Dfl_Route(ip_src, ip_dst, out_sw_port, slot_no, redis_ip);
            snprintf(cmd, CMD_MAX_LENGHT, "rpush dflrt_%s%s_%02d_1 %s",  ip_src, ip_dst, slot_no, out_sw_port);
            if(context == NULL)
            {
                do{
                    context = NULL;
                    ret = redis_connect(&context, redis_ip); 
                    usleep(3000);
                }while(ret == FAILURE);
            }
            
            reply = (redisReply *)redisCommand(context, cmd);
            if (NULL == reply)
            {
                printf("%d execute command:%s failure\n", __LINE__, cmd);
                redisFree(context);
                return FAILURE;
            }
            freeReplyObject(reply);
            printf("%d execute command:%s success\n", __LINE__, cmd);

            // d2d_2
            fscanf(fp, "%d", &sw1);
            fscanf(fp, "%d", &sw2);
            fgets(out_sw_port, CMD_MAX_LENGHT, fp);
            out_sw_port[strlen(out_sw_port)-1]='\0';
            
            snprintf(ip_src, addr_len, "c0a844%02x", sw1+1); // 192.168.68.X
            snprintf(ip_dst, addr_len, "c0a844%02x", sw2+1);

            // Set_Dfl_Route(ip_src, ip_dst, out_sw_port, slot_no, redis_ip);
            snprintf(cmd, CMD_MAX_LENGHT, "rpush dflrt_%s%s_%02d_2 %s",  ip_src, ip_dst, slot_no, out_sw_port);
            if(context == NULL)
            {
                do{
                    context = NULL;
                    ret = redis_connect(&context, redis_ip); 
                    usleep(3000);
                }while(ret == FAILURE);
            }
            
            reply = (redisReply *)redisCommand(context, cmd);
            if (NULL == reply)
            {
                printf("%d execute command:%s failure\n", __LINE__, cmd);
                redisFree(context);
                return FAILURE;
            }
            freeReplyObject(reply);
            printf("%d execute command:%s success\n", __LINE__, cmd);


            // add link-route map
            // if(slot_init == slot_no)
            // {
            //     hop = 0;
            //     do{
            //         strncpy(sw_port, out_sw_port+hop*7+1, 7);
            //         sw = atol(sw_port);
            //         sw1 = sw/1000;
            //         sw2 = sw%1000;

            //         // Add_Rt_Set(sw1, sw2, ip_src, ip_dst, redis_ip);
            //         snprintf(cmd, CMD_MAX_LENGHT, "sadd rt_set_%02d_%02d %s%s", sw1, sw2, ip_src, ip_dst);
            //         if(context == NULL)
            //         {
            //             do{
            //                 context = NULL;
            //                 ret = redis_connect(&context, redis_ip); 
            //                 usleep(3000);
            //             }while(ret == FAILURE);
            //         }
                    
            //         reply = (redisReply *)redisCommand(context, cmd);
            //         if (NULL == reply)
            //         {
            //             printf("%d execute command:%s failure\n", __LINE__, cmd);
            //             redisFree(context);
            //             return FAILURE;
            //         }
            //         freeReplyObject(reply);
            //         printf("%d execute command:%s success\n", __LINE__, cmd);

            //         hop++;
            //     }while(*(out_sw_port+hop*7+1) == '0');
            // }
        }
    }
    fclose(fp);
    redisFree(context);
    return SUCCESS;
}


// RET_RESULT wr_dfl_c2s(int slot_no, FILE *fp, char* redis_ip)
// {
//     int j, db, ctrl, ret, num=66, sw1, sw2, k;
//     char cmd[CMD_MAX_LENGHT] = {0};
//     char out_sw_port[CMD_MAX_LENGHT] = {0};
//     char ip_src[addr_len]  = {0};
//     char ip_dst[addr_len]  = {0};
//     redisContext *context = NULL;
//     redisReply *reply = NULL;

//     while(ret == FAILURE)
//     {
//         context = NULL;
//         ret = redis_connect(&context, redis_ip); 
//         usleep(3000);
//     }
//     while(!feof(fp))
//     {
//         fscanf(fp, "%d", &sw1);
//         fscanf(fp, "%d", &sw2);
//         fgets(out_sw_port, CMD_MAX_LENGHT, fp);
//         snprintf(ip_src, addr_len, "c0a843%02x", sw1+1); // 192.168.67.X
//         snprintf(ip_dst, addr_len, "c0a842%02x", sw2+1); // 192.168.66.X
//         // Set_Dfl_Route(ip_src, ip_dst, out_sw_port, i, redis_ip);
//         snprintf(cmd, CMD_MAX_LENGHT, "rpush dflrt_%s%s_%02d %s", ip_src, ip_dst, slot_no, out_sw_port);
//         if(context == NULL)
//         {
//             do{
//                 context = NULL;
//                 ret = redis_connect(&context, redis_ip); 
//                 usleep(3000);
//             }while(ret == FAILURE);
//         }
//         reply = (redisReply *)redisCommand(context, cmd);
//         if (NULL == reply)
//         {
//             printf("%d execute command:%s failure\n", __LINE__, cmd);
//             redisFree(context);
//             return FAILURE;
//         }
//         freeReplyObject(reply);
//         printf("%d execute command:%s success\n", __LINE__, cmd);

//         fscanf(fp, "%d", &sw1);
//         fscanf(fp, "%d", &sw2);
//         fgets(out_sw_port, CMD_MAX_LENGHT, fp);
//         snprintf(ip_src, addr_len, "c0a842%02x", sw1+1); // 192.168.66.X
//         snprintf(ip_dst, addr_len, "c0a843%02x", sw2+1); // 192.168.67.X
//         // Set_Dfl_Route(ip_src, ip_dst, out_sw_port, i, redis_ip);
//         snprintf(cmd, CMD_MAX_LENGHT, "rpush dflrt_%s%s_%02d %s", ip_src, ip_dst, slot_no, out_sw_port);
//         if(context == NULL)
//         {
//             do{
//                 context = NULL;
//                 ret = redis_connect(&context, redis_ip); 
//                 usleep(3000);
//             }while(ret == FAILURE);
//         }
//         reply = (redisReply *)redisCommand(context, cmd);
//         if (NULL == reply)
//         {
//             printf("%d execute command:%s failure\n", __LINE__, cmd);
//             redisFree(context);
//             return FAILURE;
//         }
//         freeReplyObject(reply);
//         printf("%d execute command:%s success\n", __LINE__, cmd);
//     }
    
//     fclose(fp);
//     redisFree(context);
//     return SUCCESS;
// }

RET_RESULT wr_dfl_c2d(int slot_no, FILE *fp, char* redis_ip)
{
    int j, db, ctrl, ret, num=66, sw1, sw2, k, sw;
    char cmd[CMD_MAX_LENGHT] = {0};
    char out_sw_port[CMD_MAX_LENGHT] = {0}; // 存储出端口列表
    char sw_port[8] = {0,}; // 存储出端口
    int hop = 0;
    char ip_src[addr_len]  = {0};
    char ip_dst[addr_len]  = {0};
    redisContext *context = NULL;
    redisReply *reply = NULL;

    while(ret == FAILURE)
    {
        context = NULL;
        ret = redis_connect(&context, redis_ip); 
        usleep(3000);
    }
    while(!feof(fp))
    {
        fscanf(fp, "%d", &sw1);
        fscanf(fp, "%d", &sw2);
        fgets(out_sw_port, CMD_MAX_LENGHT, fp);
        out_sw_port[strlen(out_sw_port)-1]='\0';

        snprintf(ip_src, addr_len, "c0a844%02x", sw1+1); // 192.168.68.X
        snprintf(ip_dst, addr_len, "c0a842%02x", sw2+1); // 192.168.66.X
        // Set_Dfl_Route(ip_src, ip_dst, out_sw_port, i, redis_ip);
        snprintf(cmd, CMD_MAX_LENGHT, "rpush dflrt_%s%s_%02d_1 %s", ip_src, ip_dst, slot_no, out_sw_port);
        if(context == NULL)
        {
            do{
                context = NULL;
                ret = redis_connect(&context, redis_ip); 
                usleep(3000);
            }while(ret == FAILURE);
        }
        reply = (redisReply *)redisCommand(context, cmd);
        if (NULL == reply)
        {
            printf("%d execute command:%s failure\n", __LINE__, cmd);
            redisFree(context);
            return FAILURE;
        }
        freeReplyObject(reply);
        printf("%d execute command:%s success\n", __LINE__, cmd);

        // add link-route map
        // if(slot_init == slot_no)
        // {
        //     hop = 0;
        //     do{
        //         strncpy(sw_port, out_sw_port+hop*7+1, 7);
        //         sw = atol(sw_port);
        //         sw1 = sw/1000;
        //         sw2 = sw%1000;

        //         // Add_Rt_Set(sw1, sw2, ip_src, ip_dst, redis_ip);
        //         snprintf(cmd, CMD_MAX_LENGHT, "sadd rt_set_%02d_%02d %s%s", sw1, sw2, ip_src, ip_dst);
        //         if(context == NULL)
        //         {
        //             do{
        //                 context = NULL;
        //                 ret = redis_connect(&context, redis_ip); 
        //                 usleep(3000);
        //             }while(ret == FAILURE);
        //         }
                
        //         reply = (redisReply *)redisCommand(context, cmd);
        //         if (NULL == reply)
        //         {
        //             printf("%d execute command:%s failure\n", __LINE__, cmd);
        //             redisFree(context);
        //             return FAILURE;
        //         }
        //         freeReplyObject(reply);
        //         printf("%d execute command:%s success\n", __LINE__, cmd);

        //         hop++;
        //     }while(*(out_sw_port+hop*7+1) == '0');
        // }

        fscanf(fp, "%d", &sw1);
        fscanf(fp, "%d", &sw2);
        fgets(out_sw_port, CMD_MAX_LENGHT, fp);
        out_sw_port[strlen(out_sw_port)-1]='\0';
        
        snprintf(ip_src, addr_len, "c0a842%02x", sw1+1); // 192.168.66.X
        snprintf(ip_dst, addr_len, "c0a844%02x", sw2+1); // 192.168.68.X
        // Set_Dfl_Route(ip_src, ip_dst, out_sw_port, i, redis_ip);
        snprintf(cmd, CMD_MAX_LENGHT, "rpush dflrt_%s%s_%02d_1 %s", ip_src, ip_dst, slot_no, out_sw_port);
        if(context == NULL)
        {
            do{
                context = NULL;
                ret = redis_connect(&context, redis_ip); 
                usleep(3000);
            }while(ret == FAILURE);
        }
        reply = (redisReply *)redisCommand(context, cmd);
        if (NULL == reply)
        {
            printf("%d execute command:%s failure\n", __LINE__, cmd);
            redisFree(context);
            return FAILURE;
        }
        freeReplyObject(reply);
        printf("%d execute command:%s success\n", __LINE__, cmd);

        // add link-route map
        // if(slot_init == slot_no)
        // {
        //     hop = 0;
        //     do{
        //         strncpy(sw_port, out_sw_port+hop*7+1, 7);
        //         sw = atol(sw_port);
        //         sw1 = sw/1000;
        //         sw2 = sw%1000;

        //         // Add_Rt_Set(sw1, sw2, ip_src, ip_dst, redis_ip);
        //         snprintf(cmd, CMD_MAX_LENGHT, "sadd rt_set_%02d_%02d %s%s", sw1, sw2, ip_src, ip_dst);
        //         if(context == NULL)
        //         {
        //             do{
        //                 context = NULL;
        //                 ret = redis_connect(&context, redis_ip); 
        //                 usleep(3000);
        //             }while(ret == FAILURE);
        //         }
                
        //         reply = (redisReply *)redisCommand(context, cmd);
        //         if (NULL == reply)
        //         {
        //             printf("%d execute command:%s failure\n", __LINE__, cmd);
        //             redisFree(context);
        //             return FAILURE;
        //         }
        //         freeReplyObject(reply);
        //         printf("%d execute command:%s success\n", __LINE__, cmd);

        //         hop++;
        //     }while(*(out_sw_port+hop*7+1) == '0');
        // }
    }
    
    fclose(fp);
    redisFree(context);
    return SUCCESS;
}

int main(int argc,char *argv[])
{
    uint32_t sw, sw1, sw2;
    uint32_t ctrl, ctrl1, ctrl2;
    uint32_t db;
    uint64_t delay;
    float dist;
    int i, j, k = 0;
    int num = 66;
    FILE *fp, *fp1, *fp2 = NULL;
    char fname[fname_len] = {0,};
    char out_sw_port[CMD_MAX_LENGHT] = {0,};
    char ip_src[addr_len]  = {0,};
    char ip_dst[addr_len]  = {0,};
    // char redis_ip[redis_ip_len] = {0,};
    int nodeid = DB_ID; // 数据库所在节点序号
    char cmd[CMD_MAX_LENGHT] = {0};
    uint64_t sw_tmp = 0;
    // int slot_init = atol(argv[1]);
    char *redis_ip = argv[1];

    // read local ip
    // snprintf(redis_ip, redis_ip_len, "192.168.68.%d", nodeid+1);
    // snprintf(redis_ip, redis_ip_len, "192.168.10.118");

    // write topo: uint32_t sw1, uint32_t sw2, uint64_t delay
    for(i = 0; i < slot_num; i++)
    {
        snprintf(fname, fname_len, "../proj_topo/test/test_%d", i);
        if((fp=fopen(fname,"r"))==NULL)
        {
            printf("打开文件%s错误\n", fname);
            return -1;
        }
        wr_topo(i, fp, redis_ip);
    }

    // // write switch <-> controller(active and standby): uint32_t sw, uint32_t ctrl
    // for(i = 0; i < slot_num; i++)
    // {
    //     snprintf(fname, fname_len, "../proj_topo/active_ctrl_%d", i);
    //     if((fp1=fopen(fname,"r"))==NULL)
    //     {
    //         printf("打开文件%s错误\n", fname);
    //         return -1;
    //     }
        
    //     snprintf(fname, fname_len, "../proj_topo/standby_ctrl_%d", i);
    //     if((fp2=fopen(fname,"r"))==NULL)
    //     {
    //         printf("打开文件%s错误\n", fname);
    //         return -1;
    //     }

    //     wr_sw2ctrl_active_standby(i, fp1, fp2, redis_ip);
    // }

    // write controller <-> database: uint32_t ctrl, uint32_t db
    // for(i = 0; i < slot_num; i++)
    // {
    //     snprintf(fname, fname_len, "../proj_topo/db_conn_ctrl/db_%d", i);
    //     if((fp=fopen(fname,"r"))==NULL)
    //     {
    //         printf("打开文件%s错误\n", fname);
    //         return -1;
    //     }

    //     wr_Ctrl_Conn_Db(i, fp, redis_ip);
    // }

    // write default routes: char *ip_src, char *ip_dst, char *out_sw_port
    for(i = 0; i < slot_num; i++)
    {
        // // s2s default routes
        // snprintf(fname, fname_len, "../proj_topo/s2s_%d", i);
        // if((fp=fopen(fname,"r"))==NULL)
        // {
        //     printf("打开文件%s错误\n", fname);
        //     return -1;
        // }

        // wr_dfl_s2s(i, fp, redis_ip);

        // d2d default routes
        snprintf(fname, fname_len, "../proj_topo/route_d2d/d2d_%d", i);
        if((fp=fopen(fname,"r"))==NULL)
        {
            printf("打开文件%s错误\n", fname);
            return -1;
        }

        wr_dfl_d2d(i, fp, redis_ip);

        // // c2s default routes
        // snprintf(fname, fname_len, "../proj_topo/c2s_%d", i);
        // if((fp=fopen(fname,"r"))==NULL)
        // {
        //     printf("打开文件%s错误\n", fname);
        //     return -1;
        // }

        // wr_dfl_c2s(i ,fp, redis_ip);

        // c2d default routes
        snprintf(fname, fname_len, "../proj_topo/route_c2d/c2d_%d", i);
        if((fp=fopen(fname,"r"))==NULL)
        {
            printf("打开文件%s错误\n", fname);
            return -1;
        }

        wr_dfl_c2d(i, fp, redis_ip);
    }

    // write links that next slot will be deleted: uint32_t sw1, uint32_t sw2
    for(i = 0; i < slot_num; i++)
    {
        snprintf(fname, fname_len, "../proj_topo/del_link_set/del_link_%d", i);
        if((fp=fopen(fname,"r"))==NULL)
        {
            printf("打开文件%s错误\n", fname);
            return -1;
        }
        getc(fp);
        if (feof(fp))continue;
        else rewind(fp);//将光标跳回到文件开头
        
        while(!feof(fp))//当文件为空时，也会返回成功
        {
            fscanf(fp, "%d %d", &sw1, &sw2);
            Set_Del_Link(sw1, sw2, i, redis_ip);
        }
        fclose(fp);
    }
    return 0;
}