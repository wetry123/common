//标准库头文件包含
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/time.h>
#include <string.h>
#include <pthread.h>
#include <string>
#include <algorithm>

#include "SysType.h"
#include "PDTrace.h"
#include "OsAbstract.h"


#include "Message.h"
#include "ini_wrapper.h"
#include <iostream>
#include <fstream>
#include <sstream>
using namespace std;

#define TEST_LOGGER

struct st_Task_Info
{
    CHAR byTaskName[16];
    CHAR byTaskPara[16];
    pid_t dwTaskID;
    time_t dwExitBootTime;
    time_t dwExitRunTime;
    pid_t dwExitTaskID;
    DWORD dwExitTimes;
    SDWORD dwExitStatus;
};

struct st_SysTask
{
    DWORD dwTaskNum;
    struct st_Task_Info st_task[7];
};

//各模块初始化函数声明
extern void kvmManagerInit(int argc, char** argv);
extern void FpgaSpiManagerInit(int argc, char** argv);
extern void UsbIpManagerInit(int argc, char** argv);
extern void QtManagerInit(int argc, char** argv);
extern void MppManagerInit(int argc, char** argv);
void HardWareManagerInit(int argc, char** argv);
void UpdateManagerInit(int argc, char** argv);
void Gsv2002ManagerInit(int argc, char** argv);
extern void DBManagerServiceInit(int argc, char** argv);
extern void DBManagerClientInit(int argc, char** argv);
/*
    打印子进程退出原因, status为wait()函数结果
    详见：man 2 wait
*/
static void ShowStatus(pid_t pid, int status)
{
    PRINT_DEBUG("[show_status] pid: %u, status: % => ", pid);
    if (WIFEXITED(status))
    {
        PRINT_DEBUG("the child terminated normally, that is, "
                    "by calling exit() or _exit(), or "
                    "by returning from main().");
        int exitCode = WEXITSTATUS(status);
        PRINT_DEBUG("the child process exit code: %d", exitCode);
    }
    else if (WIFSIGNALED(status))
    {
        int signalNo = WTERMSIG(status);
        PRINT_DEBUG("the child process terminated because of signal %d"
                    " which was not caught.", signalNo);
    }
}

/********************************************************
函数描述:
        创建进程

入口参数:
        pcPara

返回:
        SYS_ERROR - 创建失败
        pid - 子进程ID
********************************************************/

static SDWORD CreateProcess(const CHAR * pcPara, struct st_SysTask * st_TaskInfo)
{
    pid_t pid;
    BYTE byNum = 0;
    BYTE byFlg = 1;

    pid = fork();                        //创建子进程
    if(pid == -1)                        //创建失败
    {
        PRINT_FAULT("fork error\n");
        return SYS_ERROR;
    }
    else if(pid != 0)                    //如果是父进程,则返回
    {
        for(byNum = 0; byNum < st_TaskInfo->dwTaskNum; byNum++)
        {
            if(0 == strcmp(pcPara, st_TaskInfo->st_task[byNum].byTaskName))
            {
                st_TaskInfo->st_task[byNum].dwTaskID = pid;
                st_TaskInfo->st_task[byNum].dwExitBootTime = time(NULL);
                byFlg = 0;
            }
        }

        if(byFlg)
        {
            strcpy(st_TaskInfo->st_task[st_TaskInfo->dwTaskNum].byTaskName, pcPara);
            strcpy(st_TaskInfo->st_task[st_TaskInfo->dwTaskNum].byTaskPara, pcPara);
            st_TaskInfo->st_task[st_TaskInfo->dwTaskNum].dwTaskID = pid;
            st_TaskInfo->dwTaskNum++;
            st_TaskInfo->st_task[st_TaskInfo->dwTaskNum].dwExitBootTime = time(NULL);
        }

        OSTaskDelay(100);
        return pid;
    }

    //执行IPC程序文件
    if(execlp("./App","App",pcPara,(char *)0) < 0)
    {
        perror("execlp error");
        PRINT_FAULT("execlp error\n");
    }
    return pid;
}

#define APPVERSION_INI    "/nand/config/AppVersion.xml"
VOID Get_Compile_Date_Base(WORD *Year, WORD *Month, WORD *Day)
{
    const char *pMonth[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const char Date[12] = __DATE__;//取编译时间
    BYTE i;
    for(i = 0; i < 12; i++)
        if(memcmp(Date, pMonth[i], 3) == 0)
            *Month = i + 1, i = 12;
    *Year = (WORD)atoi(Date + 9); //Date[9]为２位年份，Date[7]为完整年份
    *Day = (WORD)atoi(Date + 4);
}
VOID SetAppCompileTime()
{
    remove(APPVERSION_INI);
    WORD Year,Month,Day;
    Get_Compile_Date_Base(&Year, &Month, &Day);//取编译时间
    std::string strDate = std::to_string(Year) + "-" + std::to_string(Month) + "-" + std::to_string(Day);
    IniWriteString(APPVERSION_INI, "App_Compile_Info", "Date", strDate.c_str());
    IniWriteString(APPVERSION_INI, "App_Compile_Info", "Time", __TIME__);
}

/********************************************************
函数描述:
        软件总入口

入口参数:
        void

返回:
        该函数永不返回
********************************************************/
SDWORD main(SDWORD sdwArgc, CHAR * pcArg[])
{
    if(sdwArgc < 2)
    {
        printf("Usage: ./App [process name]\n");
        exit(1);
    }

    struct st_SysTask st_TaskInfo;
    BYTE byTaskNum = 0;
    pid_t waitpid;

    //忽略SIGPIPE，防止其导致进程退出
    signal(SIGPIPE, SIG_IGN);

    memset(&st_TaskInfo, 0, sizeof(st_TaskInfo));
    if(0 == strcmp(pcArg[1], "Main"))
    {
        SetAppCompileTime();

        printf("APP Start!  @ %s %s\n", __DATE__, __TIME__);
        PDInitTrace(PD_DEFAULT_CFG_PATH, pcArg[1]);

        ifstream file;
        file.open("/nand/version/swversion", ios_base::in);

        if (file.is_open())
        {
            stringstream ss;
            ss << file.rdbuf();
            file.close();

            string strVersion = ss.str();
            strVersion.erase(std::remove(strVersion.begin(),  strVersion.end(),  '\n') ,  strVersion.end());
            strVersion.erase(std::remove(strVersion.begin(),  strVersion.end(),  '\r') ,  strVersion.end());
            IniWriteString(DEVINFO_INI, "BaseInfo", "SwVersion", strVersion.c_str());
            IniWriteString(DEVINFO_INI, "BaseInfo", "HwVersion", strVersion.c_str());
        }

        PRINT_TRACE("APP Start!  @ %s %s\n", __DATE__, __TIME__);
        // GSV2002 FIRST
        CreateProcess("Gsv2002", &st_TaskInfo);

        // 升级烧录模块
        CreateProcess("Update", &st_TaskInfo);
        // 基本外设
        CreateProcess("HardWare", &st_TaskInfo);
        // 键鼠管理模块
        CreateProcess("Kvm", &st_TaskInfo);
        OSTaskDelay(10*1000);

        // 数据库管理 ---->16进制协议停用，暂时去掉数据库管理进程
        //CreateProcess("DBService", &st_TaskInfo);

        // SPI数据处理管理模块
        CreateProcess("FpgaSpi", &st_TaskInfo);
        OSTaskDelay(3*1000);

        // Mpp管理模块
        CreateProcess("Mpp", &st_TaskInfo);
        OSTaskDelay(1*1000);
    }
    else if(0 == strcmp(pcArg[1], "USBIP"))
    {
#ifdef TEST_LOGGER
        PDInitTrace(PD_DEFAULT_CFG_PATH,  pcArg[1]);

        for(int i=0; i<10; i++)
        {
            PRINT_LOGFILE(LOG_TYPE_CMD, "TEST CMD LOGGER FUNCTION!     -------------> %s[%d]", pcArg[1], i);
            PRINT_LOGFILE(LOG_TYPE_WORK, "TEST WORK LOGGER FUNCTION!     -------------> %s[%d]", pcArg[1], i);
        }

        char logmsg[1024] = {0};

        SCAN_LOGFILE(LOG_TYPE_CMD, logmsg, 8);
        printf("cmd losmsg:\n%s\n", logmsg);
        bzero(logmsg, 1024);
        SCAN_LOGFILE(LOG_TYPE_WORK, logmsg, 20);
        printf("work losmsg:\n%s\n", logmsg);
#endif
        UsbIpManagerInit(sdwArgc, pcArg);
    }
    else if(0 == strcmp(pcArg[1], "FpgaSpi"))
    {
        printf("Start FpgaSpi\n");
        FpgaSpiManagerInit(sdwArgc, pcArg);
    }
    else if(0 == strcmp(pcArg[1], "Kvm"))
    {
        printf("Start Kvm\n");
        kvmManagerInit(sdwArgc, pcArg);
    }
    else if(0 == strcmp(pcArg[1], "Qt"))
    {
        printf("Start Qt\n");
        QtManagerInit(sdwArgc, pcArg);
    }
    else if(0 == strcmp(pcArg[1], "HardWare"))
    {
        printf("Start HardWare\n");
        HardWareManagerInit(sdwArgc, pcArg);
    }
    else if(0 == strcmp(pcArg[1], "Update"))
    {
        printf("Start Update\n");
        UpdateManagerInit(sdwArgc, pcArg);
    }
    else if(0 == strcmp(pcArg[1],"Mpp"))
    {
        printf("Start Mpp\n");
        MppManagerInit(sdwArgc, pcArg);
    }
    else if(0 == strcmp(pcArg[1],"DBService"))
    {
        printf("Start DBService\n");
        // DBManagerServiceInit(sdwArgc, pcArg);
    }
    else if(0 == strcmp(pcArg[1],"DBClient"))
    {
        printf("Start DBClient\n");
        // DBManagerClientInit(sdwArgc, pcArg);
    }
    else if(0 == strcmp(pcArg[1],"Gsv2002"))
    {
        printf("Start Gsv2002\n");
        Gsv2002ManagerInit(sdwArgc, pcArg);
    }
    else
    {
        printf("Can`t find process name!\n");
        exit(1);
    }


    //监控子进程退出并重启进程
    while(1)
    {

        PRINT_DEBUG("*****************************************\n");
        PRINT_DEBUG("Waiting for the child process terminated!\n");
        PRINT_DEBUG("*****************************************\n");
        int status = 0;
        waitpid = wait(&status);
        //过滤进程正常退出的打印
        if(status == 0)
            continue;
        PRINT_DEBUG("*****************************************\n");
        PRINT_DEBUG("The ID of the child process terminated: %d\n",waitpid);
        PRINT_DEBUG("*****************************************\n");
        ShowStatus(waitpid, status);
        for(byTaskNum = 0; byTaskNum < st_TaskInfo.dwTaskNum; byTaskNum++)
        {
            if(st_TaskInfo.st_task[byTaskNum].dwTaskID == waitpid)
            {
                st_TaskInfo.st_task[byTaskNum].dwExitStatus = status;
                st_TaskInfo.st_task[byTaskNum].dwExitTaskID = waitpid;
                st_TaskInfo.st_task[byTaskNum].dwExitTimes++;
                st_TaskInfo.st_task[byTaskNum].dwExitRunTime = time(NULL) -
                        st_TaskInfo.st_task[byTaskNum].dwExitBootTime;
                PRINT_DEBUG("ExitTaskName:[%s], ExitStatus:[%d], dwExitTaskID:[%d], dwExitTimes:[%d], dwExitRunTime:[%d]\n",
                            st_TaskInfo.st_task[byTaskNum].byTaskName,st_TaskInfo.st_task[byTaskNum].dwExitStatus,
                            st_TaskInfo.st_task[byTaskNum].dwExitTaskID,st_TaskInfo.st_task[byTaskNum].dwExitTimes,
                            st_TaskInfo.st_task[byTaskNum].dwExitRunTime);

                if(st_TaskInfo.st_task[byTaskNum].dwExitRunTime < 10)
                {
                    OSTaskDelay(1000);
                }

                CreateProcess(st_TaskInfo.st_task[byTaskNum].byTaskPara, &st_TaskInfo);
            }
        }
        if (byTaskNum == st_TaskInfo.dwTaskNum)
        {
            PRINT_DEBUG("Cannot found the info of pid %d", waitpid);
        }
    }
}


