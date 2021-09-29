// init database content

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "db_wr.h"
// #include "db_wr.c"

#define slot_num 44
#define db_num 6
#define fname_len 50
#define addr_len 8
#define redis_ip_len 20
#define DB_ID 1 // database_id = 192.168.68.2

int main(int argc,char *argv[])
{
    uint32_t sw, sw1, sw2;
    uint32_t ctrl, ctrl1, ctrl2;
    uint32_t db;
    uint64_t delay;
    float dist;
    int i, j, k = 0;
    int num = 0;
    FILE *fp, *fp1, *fp2 = NULL;
    char fname[fname_len] = {0,};
    char out_sw_port[CMD_MAX_LENGHT] = {0,};
    char ip_src[addr_len]  = {0,};
    char ip_dst[addr_len]  = {0,};
    char redis_ip[redis_ip_len] = {0,};
    int nodeid = DB_ID; // 数据库所在节点序号

    // read local ip
    // snprintf(redis_ip, redis_ip_len, "192.168.68.%d", nodeid+1);
    snprintf(redis_ip, redis_ip_len, "192.168.10.118");

    // write topo: uint32_t sw1, uint32_t sw2, uint64_t delay
    for(i = 0; i < slot_num; i++)
    {
        snprintf(fname, fname_len, "../proj_topo/test_%d", i);
        if((fp=fopen(fname,"r"))==NULL)
        {
            printf("打开文件%s错误\n", fname);
            return -1;
        }
        fscanf(fp, "%d", &num);

        while(!feof(fp))
        {
            fscanf(fp, "%d %d %f", &sw1, &sw2, &dist);
            delay = (int)(dist*1e5);
            Set_Topo(sw1, sw2, delay, i, (char*)redis_ip);
        }
        fclose(fp);

        for(sw = 0; sw < num; sw++)
        {
            Set_Topo(sw, sw, 0, i, redis_ip);
        }
    }

    // write switch <-> controller(active and standby): uint32_t sw, uint32_t ctrl
    for(i = 0; i < slot_num; i++)
    {
        snprintf(fname, fname_len, "../proj_topo/active_ctrl_%d", i);
        if((fp1=fopen(fname,"r"))==NULL)
        {
            printf("打开文件%s错误\n", fname);
            return -1;
        }
        fscanf(fp1, "%d\n", &num);
        
        snprintf(fname, fname_len, "../proj_topo/standby_ctrl_%d", i);
        if((fp2=fopen(fname,"r"))==NULL)
        {
            printf("打开文件%s错误\n", fname);
            return -1;
        }
        fscanf(fp2, "%d\n", &num);

        for(sw = 0; sw < num; sw++)
        {
            fscanf(fp1, "%d", &ctrl1);
            fscanf(fp2, "%d", &ctrl2);

            Set_Active_Ctrl(sw, ctrl1, i, redis_ip);
            Set_Standby_Ctrl(sw, ctrl2, i, redis_ip);
        }

        fclose(fp1);
        fclose(fp2);
    }

    // write controller <-> database: uint32_t ctrl, uint32_t db
    for(i = 0; i < slot_num; i++)
    {
        snprintf(fname, fname_len, "../proj_topo/db_%d", i);
        if((fp=fopen(fname,"r"))==NULL)
        {
            printf("打开文件%s错误\n", fname);
            return -1;
        }

        for(j = 0; j < db_num; j++)
        {
            fscanf(fp, "%d", &db);
            fgetc(fp); // read '\n'
            while(fgetc(fp) == ' ')
            {
                fscanf(fp, "%d", &ctrl);
                Set_Ctrl_Conn_Db(ctrl, db, i, redis_ip);
            }
        }

        fclose(fp);
    }

    // write default routes: char *ip_src, char *ip_dst, char *out_sw_port
    for(i = 0; i < slot_num; i++)
    {
        // s2s default routes
        snprintf(fname, fname_len, "../proj_topo/s2s_%d", i);
        if((fp=fopen(fname,"r"))==NULL)
        {
            printf("打开文件%s错误\n", fname);
            return -1;
        }

        for(j = 0; j < num; j++)
        {
            fscanf(fp, "%d", &sw1);
            for(k = 1; k < num; k++)
            {
                fscanf(fp, "%d", &sw1);
                fscanf(fp, "%d", &sw2);
                fgets(out_sw_port, CMD_MAX_LENGHT, fp);
               
                snprintf(ip_src, addr_len, "C0A842%02x", sw1+1); // 192.168.66.X
                snprintf(ip_dst, addr_len, "C0A842%02x", sw2+1);

                Set_Dfl_Route(ip_src, ip_dst, out_sw_port, i, redis_ip);
            }
        }
        fclose(fp);

        // d2d default routes
        snprintf(fname, fname_len, "../proj_topo/d2d_%d", i);
        if((fp=fopen(fname,"r"))==NULL)
        {
            printf("打开文件%s错误\n", fname);
            return -1;
        }

        for(j = 0; j < db_num; j++)
        {
            fscanf(fp, "%d", &sw1);
            for(k = 1; k < db_num; k++)
            {
                fscanf(fp, "%d", &sw1);
                fscanf(fp, "%d", &sw2);
                fgets(out_sw_port, CMD_MAX_LENGHT, fp);
               
                snprintf(ip_src, addr_len, "C0A844%02x", sw1+1); // 192.168.68.X
                snprintf(ip_dst, addr_len, "C0A844%02x", sw2+1);

                Set_Dfl_Route(ip_src, ip_dst, out_sw_port, i, redis_ip);
            }
        }
        fclose(fp);

        // c2s default routes
        snprintf(fname, fname_len, "../proj_topo/c2s_%d", i);
        if((fp=fopen(fname,"r"))==NULL)
        {
            printf("打开文件%s错误\n", fname);
            return -1;
        }

        while(!feof(fp))
        {
            fscanf(fp, "%d", &sw1);
            fscanf(fp, "%d", &sw2);
            fgets(out_sw_port, CMD_MAX_LENGHT, fp);
            snprintf(ip_src, addr_len, "C0A843%02x", sw1+1); // 192.168.67.X
            snprintf(ip_dst, addr_len, "C0A842%02x", sw2+1); // 192.168.66.X
            Set_Dfl_Route(ip_src, ip_dst, out_sw_port, i, redis_ip);

            fscanf(fp, "%d", &sw1);
            fscanf(fp, "%d", &sw2);
            fgets(out_sw_port, CMD_MAX_LENGHT, fp);
            snprintf(ip_src, addr_len, "C0A842%02x", sw1+1); // 192.168.66.X
            snprintf(ip_dst, addr_len, "C0A843%02x", sw2+1); // 192.168.67.X
            Set_Dfl_Route(ip_src, ip_dst, out_sw_port, i, redis_ip);
        }
        fclose(fp);

        // c2d default routes
        snprintf(fname, fname_len, "../proj_topo/c2d_%d", i);
        if((fp=fopen(fname,"r"))==NULL)
        {
            printf("打开文件%s错误\n", fname);
            return -1;
        }

        while(!feof(fp))
        {
            fscanf(fp, "%d", &sw1);
            fscanf(fp, "%d", &sw2);
            fgets(out_sw_port, CMD_MAX_LENGHT, fp);
            snprintf(ip_src, addr_len, "C0A844%02x", sw1+1); // 192.168.68.X
            snprintf(ip_dst, addr_len, "C0A843%02x", sw2+1); // 192.168.67.X
            Set_Dfl_Route(ip_src, ip_dst, out_sw_port, i, redis_ip);

            fscanf(fp, "%d", &sw1);
            fscanf(fp, "%d", &sw2);
            fgets(out_sw_port, CMD_MAX_LENGHT, fp);
            snprintf(ip_src, addr_len, "C0A843%02x", sw1+1); // 192.168.67.X
            snprintf(ip_dst, addr_len, "C0A844%02x", sw2+1); // 192.168.68.X
            Set_Dfl_Route(ip_src, ip_dst, out_sw_port, i, redis_ip);
        }
        fclose(fp);
    }

    // write links that next slot will be deleted: uint32_t sw1, uint32_t sw2
    for(i = 0; i < slot_num; i++)
    {
        snprintf(fname, fname_len, "../proj_topo/del_link_%d", i);
        if((fp=fopen(fname,"r"))==NULL)
        {
            printf("打开文件%s错误\n", fname);
            return -1;
        }

        while(!feof(fp))
        {
            fscanf(fp, "%d %d", &sw1, &sw2);
            Set_Del_Link(sw1, sw2, i, redis_ip);
        }
        fclose(fp);
    }
    return 0;
}