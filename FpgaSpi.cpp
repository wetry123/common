#include "FpgaSpiManager.h"
#include <iostream>
#include <string.h>
#include <unistd.h>
#include <linux/input.h>
#include <sys/poll.h>
#include <sys/ioctl.h>
#include <poll.h>

#include <sys/ioctl.h>
#include <net/if.h>    /* for ifconf */
#include <linux/sockios.h>  /* for net status mask */
#include <netinet/in.h>    /* for sockaddr_in */
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/ioctl.h>   

#include "Utility.h"
#include "FpgaSpiCmdAdapter.h"
#include "FrameQueueRefactor.h"
#include "AutoLock.h"
#include "PDTrace.h"
#include "TelnetTrace.h"
#include "OsAbstract.h"
#include "msQ.h"
#include "gpio.h"
#include "gsv2002.h"

#include "FpgaSpiServer.h"
#include "FpgaSpiServerHandler.h"
#include "Message.h"
#include "Sender.h"

static char g_aPviAck[] = {
    0x42, 0x4C, 0x6D, 0x07,
    0x01, 0x00, 0x00, 0x77
};

// SPI_INT_STATUS，描述中断的状态
static WORD g_wFpgainterruptState = 0;
#define E_FPGA_INT_STATUS 	FpgaSpiReadData(SPI_INT_STATUS, &g_wFpgainterruptState, 2);

// SPI_REGADDR_STATUS，描述光纤有效状态
static FPGA_FirbStat g_FirState = FIRB_INVALID_ALL;	  
static uint32_t g_FpgaInt = 0;

// SPI_INT_DISABLE, 描述中断有无效的状态
static WORD g_wIntDisable = 0;

#define GPIO_EVENTS 1

// GPIO中断总开关
static FPGA_Ctrl_RECV 	g_Ctrl;
#define E_FPGA_INT_STOP if(g_Ctrl!=FPGA_RECV_START)FpgaSpiIntCtrl(FPGA_RECV_START),g_Ctrl=FPGA_RECV_START;
#define E_FPGA_INT_RES	if(g_Ctrl!=FPGA_RECV_END)FpgaSpiIntCtrl(FPGA_RECV_END),g_Ctrl=FPGA_RECV_END;

extern FPGA_DEV_S g_stFpgaDev;

FpgaSpiManager* FpgaSpiManager::m_pInstance = NULL;

FpgaSpiManager::FpgaSpiManager()
{
	m_pFpgaSpiCmdAdpter = new FpgaSpiCmdAdapter();
    //m_pFpgaSpiAdpter = new FpgaSpiAdapter();
	m_pMouseMessageQueue = NULL;
    m_pKmrHidRecvQueue = NULL;
	m_QtClient = NULL;
	m_UpdateClient = NULL;
	m_HardWareClient = NULL;
}

FpgaSpiManager::~FpgaSpiManager()
{
    printf("fpga spi manager distruct.\n");

    CLOSE_FD(m_FpgaDevFd);
    DELETE_PTR(m_pMouseMessageQueue);
    DELETE_PTR(m_pKmrHidRecvQueue);
    DELETE_PTR(m_QtClient);
    DELETE_PTR(m_UpdateClient);
    DELETE_PTR(m_HardWareClient);
}



void FpgaSpiManager::InitThrift()
{
    using namespace ::apache::thrift;
    using namespace ::apache::thrift::protocol;
    using namespace ::apache::thrift::transport;
    using namespace ::apache::thrift::server;    
    using namespace ::apache::thrift::concurrency;
    using namespace ::apache::thrift::stdcxx;
	
    m_UpdateClient = ThriftCommon::CreateClientInstance<UpdateServerClient>(UPDATE_THRIFT_SOCKET_PATH, 10 * 1000);
    m_QtClient = ThriftCommon::CreateClientInstance<QtServerClient>(QT_THRIFT_SOCKET_PATH, 10 * 1000);
	m_HardWareClient = ThriftCommon::CreateClientInstance<HardWareServerClient>(HARDWARE_THRIFT_SOCKET_PATH, 10 * 1000);
    m_KvmClient  = ThriftCommon::CreateClientInstance<KvmServerClient>(KVM_THRIFT_SOCKET_PATH, 2 * 1000);
	m_Gsv2002Client = ThriftCommon::CreateClientInstance<Gsv2002ServerClient>(GSV2002_THRIFT_SOCKET_PATH, 10 * 1000);
	//ThriftCommon::WaitOtherProcess<UpdateServerClient>(m_UpdateClient );

    if(m_HardWareClient == NULL || m_UpdateClient == NULL || m_KvmClient == NULL || m_Gsv2002Client == NULL || m_QtClient == NULL)
	{
        PRINT_FAULT("m_HardWareClient == NULL|| m_UpdateClient == NULL || m_KvmClient == NULL || m_Gsv2002Client == NULL");
		exit(1);
	}

    //给其他模块设置ID
    bool b=true;
    std::string key = "ID";
    std::string ID = std::to_string(GetDevID());

    ThriftCommon::ThriftCall<HardWareServerClient, const std::string &, const std::string & >
            (b, m_HardWareClient, &HardWareServerClient::LedSetString, key, ID);
    PRINT_DEBUG("****************************************\n");
    if(b) {
        PRINT_FAULT("call hardware thrift LedSetString %s %s failed!\n", key.c_str(), ID.c_str());
    } else {
        PRINT_DEBUG("call hardware thrift LedSetString %s %s successed!\n", key.c_str(), ID.c_str());
    }
    PRINT_DEBUG("****************************************\n");

    //ThriftCommon::ThriftCall<KvmServerClient, int64_t>(b, m_KvmClient, &KvmServerClient::SetDevId, GetDevID());
    //PRINT_DEBUG("****************************************\n");
    //if(b) {
    //    PRINT_FAULT("call kvm thrift SetDevId %llu failed!\n", GetDevID());
    //} else {
    //    PRINT_DEBUG("call kvm thrift SetDevId %llu successed!\n", GetDevID());
    //}
    //PRINT_DEBUG("****************************************\n");

//    ThriftCommon::ThriftCall<QtServerClient>(b, m_QtClient, &QtServerClient::NotifyAppRestarted);
//    PRINT_DEBUG("****************************************\n");
//    if(b) {
//        PRINT_FAULT("call qt thrift NotifyAppRestarted failed!\n");
//    } else {
//        PRINT_DEBUG("call qt thrift NotifyAppRestarted successed!\n");
//    }
//    PRINT_DEBUG("****************************************\n");

    ::apache::thrift::GlobalOutput.setOutputFunction(ThriftOutputFunction);
    shared_ptr<FpgaSpiServerHandler> handler(new FpgaSpiServerHandler(this));
    shared_ptr<TProcessor> processor(new FpgaSpiServerProcessor(handler));
    ThriftCommon::thriftServerLoop(FPGASPI_THRIFT_SOCKET_PATH,processor, spTSvr);
}


void FpgaSpiManager::TaskPviGet()
{
	SDWORD         sdStatus       = FALSE;

    while (1)
    {
		// ping
		// 异常处理
		OSTaskDelay(5000);
    }

}

VOID* FpgaSpiManager::TaskPviGetEntry(VOID * p)
{
    if (p == NULL)
    {
        return NULL;
    }
    FpgaSpiManager *pThis = (FpgaSpiManager *)p;
#if 0	
	if(pThis->m_pMouseMessageQueue != NULL && TRUE != pThis->m_pMouseMessageQueue->IsEmpty())
    {
        pThis->m_pMouseMessageQueue->Flush();
    }
#endif		
    pThis->TaskPviGet();

    return NULL;
}


void FpgaSpiManager::TaskPviAck()
{
    long timeoutval = 1000 * 1000;

    while (1)
    {
        if(m_byPviAckStatus) {
            gettimeofday(&m_stGetPviDataEnd, nullptr);
            long costtime = (m_stGetPviDataEnd.tv_sec - 1 - m_stGetPviDataBegin.tv_sec) * 1000000
                + (1000000 + m_stGetPviDataEnd.tv_usec - m_stGetPviDataBegin.tv_usec);
            if(costtime < 0 || costtime > timeoutval) {
                FpgaWrite(SPI_INT_PVI_SEND_OVER, (BYTE *)g_aPviAck, sizeof(g_aPviAck));
                printf("send ack to mx..................!!!!!\n");
                gettimeofday(&m_stGetPviDataBegin, nullptr);
            }
        }
		OSTaskDelay(10);
    }
}

VOID* FpgaSpiManager::TaskPviAckEntry(VOID * p)
{
    if (p == NULL)
    {
        return NULL;
    }
    FpgaSpiManager *pThis = (FpgaSpiManager *)p;
    pThis->TaskPviAck();
    return NULL;
}


void FpgaSpiManager::TaskKvmGet()
{
	SDWORD         sdStatus       = FALSE;
	BYTE byReadUsb[4096] = {0};
	MouseMessageQueue::MouseMessageNodeDesc node;
    ITCMsgHeader *pstItcMsgHeader = (ITCMsgHeader *)byReadUsb;
	Inputevent_st *pGetInput = reinterpret_cast<Inputevent_st*>(pstItcMsgHeader + 1);
	
	bzero(&node,sizeof(node));
    
    pstItcMsgHeader->wProjectId = ITC_PROJECT_ID;
    pstItcMsgHeader->wCheckId   = ITC_CHECK_ID;
    pstItcMsgHeader->wDataLen = 0;
    pstItcMsgHeader->byReserve = 0;
    pstItcMsgHeader->bySrcDev   = DEVICE_TYPE_TV_6141RX;
    pstItcMsgHeader->byDstDev   = DEVICE_TYPE_TV_6141TX;
    pstItcMsgHeader->wCmdId     = CMDID_KVM_DATA;
            
    //清除PVI缓存
    FpgaWriteReg(SPI_CACHE_CLEAR, (0x3) << 12);

    while (1)
    {
		// 处理键鼠数据
		m_pMouseMessageQueue->Wait();
        while(SYS_OK == m_pMouseMessageQueue->GetNode(&node,(BYTE *)pGetInput))
		{
#if 0
			PRINT_DEBUG("Len:%d,TimeStamp:%llu(cur:%llu),KvmId:0x%x",node.dwDataLen, node.ddwTimeStamp, GetUnixTimeInMsec(),node.dwMouseKvmID);
			PRINT_DEBUG_BUFFER((BYTE *)pGetInput, node.dwDataLen);
#endif
			// 1、添加包头
            pstItcMsgHeader->wDataLen   = node.dwDataLen + sizeof(ITCMsgHeader) - sizeof(ITCHeader);
            //TODO:是FpgaSpiManager添加kvm数据头 还是KvmManager添加kvm数据头？
            //pstItcMsgHeader.wSession   = ?

			// 2、写数据回FPGA
        	if(g_FirState != FIRB_INVALID_ALL)
			{
                switch (node.dwNodeType) {
                case SPI_INT_HID_SEND_OVER:
//                    for(int i = 0; i < node.dwDataLen/sizeof(Inputevent_st); i++) {
//                        PRINT_DEBUG("Send fpga hid ev invalid:%d, type:%d, code:%d, value:%d.",pGetInput->byInvalid, pGetInput->stEv.type, pGetInput->stEv.code, pGetInput->stEv.value);
//                    }

                    FpgaSpiManager::GetInstance()->SendHidData((BYTE *)pstItcMsgHeader, pstItcMsgHeader->wDataLen + 7);
                    break;
                case SPI_INT_KMR_SEND_OVER:
                    FpgaSpiManager::GetInstance()->SendKmrData((BYTE *)pstItcMsgHeader, pstItcMsgHeader->wDataLen + 7);
                default:
                    PRINT_FAULT("unknown nod type:%d.", node.dwNodeType);
                    break;
                }
			}
		}
		
		//usleep(1*1000);
    }


}

VOID* FpgaSpiManager::TaskKvmGetEntry(VOID * p)
{
    if (p == NULL)
    {
        return NULL;
    }
    FpgaSpiManager *pThis = (FpgaSpiManager *)p;
	
	if(pThis->m_pMouseMessageQueue != NULL && TRUE != pThis->m_pMouseMessageQueue->IsEmpty())
    {
        pThis->m_pMouseMessageQueue->Flush();
    }
		
    pThis->TaskKvmGet();

    return NULL;
}



void FpgaSpiManager::TaskSend()
{
    BYTE *pbyBuf = new BYTE[MAXBUFSIZE];
    ITCHeader *pHeader = (ITCHeader*)pbyBuf;
	memset(pbyBuf,0,MAXBUFSIZE);
	
    while (1)
    {
        memset(pbyBuf,0,pHeader->wDataLen+13);
		// 从TaskProcess接收数据，也就是从指令处理层接收数据
        if (SYS_ERROR != msgQReceive(m_MsgQ, (CHAR *)(pbyBuf), MAXBUFSIZE, WAIT_FOREVER, FpgaSpi_MSQ_OUTPUT_DATA) )
        {
        	// 写数据回FPGA
        	//PRINT_DEBUG("wDataLen = %d\n", pMsqHeader->wDataLen + sizeof(ITCHeader));
            //PRINT_DEBUG_BUFFER(pbyBuf, pMsqHeader->wDataLen + sizeof(ITCHeader));

            switch (pHeader->wProjectId) {
                case ITC_JSON_PROJECT_ID:{
                    Json::Value root;
                    Json::Reader reader;
                    reader.parse((char *)(pbyBuf + sizeof(ITCHeader)), (char *)(pbyBuf + sizeof(ITCHeader) + pHeader->wDataLen), root);
                    if(root["actioncode"].asString() == "LogReport") {
                        root["data"]["device_id"] = (Json::UInt64)FpgaSpiManager::GetInstance()->GetCurDevID();

                        //填充发送json内容
                        Json::FastWriter writer;
                        Json::String s = writer.write(root);
                        if(s.length() > (MAXBUFSIZE-sizeof(ITCHeader))) {
                            printf("json logreport too long, len:%d.", s.length());
                            continue;
                        }
                        memcpy(pHeader+1, s.data(), s.length());

                        pHeader->wDataLen = s.length();
                    }else{
                        printf("TaskSend:\n%s", root.toStyledString().c_str());
                    }
					
                    break;
                }
                default:
                    break;
            }

            FpgaSpiManager::GetInstance()->SendCmdData(pbyBuf, pHeader->wDataLen + sizeof(ITCHeader));
        }
		else
        {
            PRINT_FAULT("FpgaSpiManager::msgQReceive error:%s\n",strerror(errno));
			OSTaskDelay(100);
            //continue;
        }


    }

    delete[] pbyBuf;
}

VOID* FpgaSpiManager::TaskSendEntry(VOID *p)
{
    if (p == NULL)
    {
        return NULL;
    }
    FpgaSpiManager *pThis = (FpgaSpiManager *)p;
    pThis->TaskSend();

    return NULL;
}

void FpgaSpiManager::TaskProcess()
{
    SDWORD sdRecvLen = SYS_ERROR;
	WORD wDataLen = 0;
    BYTE *pbyBuf = new BYTE[MAXBUFSIZE];
    BYTE *pbyUpdateResult = new BYTE[1024];
	BYTE* pMsgData = (BYTE*)(pbyBuf + sizeof(FpgaSpiMsqHeader));
	ITCMsgHeader *pMsqHeader = (ITCMsgHeader*)pbyBuf;
	ITCHeader *pJsonMsqHeader = (ITCHeader*)pbyBuf;
	BYTE* pJsonMsgData = (BYTE*)(pbyBuf + sizeof(ITCHeader));
	memset(pbyBuf,0,MAXBUFSIZE);
	
    while (1)
    {
        memset(pbyBuf,0,pMsqHeader->wDataLen+13);
        // 阻塞等待应用发送报文
		// 从TaskRecvEntry接收数据，也就是从spi接收数据
        PRINT_DEBUG("msgQreceive FpgaSpi_MSQ_INPUT_DATA");
        sdRecvLen = msgQReceive(m_MsgQ, (CHAR *)(pbyBuf), MAXBUFSIZE, WAIT_FOREVER, FpgaSpi_MSQ_INPUT_DATA);
        if (SYS_ERROR ==sdRecvLen)
        {
            PRINT_FAULT("FpgaSpiManager:: msgQReceive error:%s\n",strerror(errno));
			OSTaskDelay(100);
            continue;
        }
        PRINT_DEBUG("msgQreceive FpgaSpi_MSQ_INPUT_DATA Done!");

        //PRINT_DEBUG_BUFFER(pbyBuf, sdRecvLen);
		if(pJsonMsqHeader->wProjectId == ITC_JSON_PROJECT_ID)
		{
			if(pJsonMsqHeader->byReserve != 0)
				HandleSubPack(pbyBuf, sdRecvLen);
			else		
            	m_pFpgaSpiCmdAdpter->JsonDispatchCall(pbyBuf, sdRecvLen);
		}
		else
		{
            switch (pMsqHeader->wCmdId){
                case CMDID_UPDATE_ENTRY_REQ:
                case CMDID_UPDATE_ENTRY_RSP:   
                case CMDID_UPDATE_DATA:   
                case CMDID_UPDATE_STATUS:
                case CMDID_UPDATE_ERRDATA_CHECK:
                {
                    // 报文传输到UpdateManger模块
                    PRINT_DEBUG("SendCmdByUpdateResult1 CmdId:%04x\n",pMsqHeader->wCmdId);
                    Sender& sender = Sender::getInstance();
                    sender.SMSend(pbyBuf, sdRecvLen);
                    //TODO:以后再优化为异步处理
                    int len = 0;
                    memset(pbyUpdateResult, 0, 1024);
                    // 稍微等待一下
                    usleep(10 * 1000);
                    // 等待UpdateManger模块处理
                    int retRecv = sender.SMRecv(pbyUpdateResult, len);
                    PRINT_DEBUG("SendCmdByUpdateResult 3\n");
                    UpdateCmdResult_S* pResult = (UpdateCmdResult_S*)pbyUpdateResult;
                    if(pResult->dataLen == 0 || retRecv)break;
                    
                    PRINT_DEBUG_BUFFER(pResult->data,pResult->dataLen);
                    
                    // 发送函数 
                    SendCmd(pResult->cmdId, DEVICE_TYPE_TV_6180M, pResult->data, pResult->dataLen);
                   
                    #if 0  // hxc 221219  全部报文处理放到UpdateManger模块
                    //根据Update模块的回应发出对应到应答命令
                    SendCmdByUpdateResult(*((UpdateCmdResult_S*)pbyUpdateResult), pMsqHeader + 1);
                    #endif
                    break;
                }
                default:{
                    wDataLen = pMsqHeader->wDataLen-6;
                    // 送往指令Adpter处理层进行实际处理,只处理wData部分
                    m_pFpgaSpiCmdAdpter->DispatchCall((void*)pMsqHeader,pMsgData,wDataLen);
                    break;
                }
            }
		}
	}

    delete[] pbyBuf;
    delete[] pbyUpdateResult;
}

VOID* FpgaSpiManager::TaskProcessEntry(VOID *p)
{
    if (p == NULL)
    {
        return NULL;
    }
    FpgaSpiManager *pThis = (FpgaSpiManager *)p;
    pThis->TaskProcess();

    return NULL;
}

void FpgaSpiManager::TaskCheckStatus()
{
    sleep(5);

    static bool bFirst = true; 
    static int s32FirstCnt = 0; 
    m_notifyQtUi = true;

    Json::Value jsonObject = Json::objectValue;

    jsonObject["actioncode"] = "NtfDeviceStatusChange";
    jsonObject["device_name"] = "RX_" + std::to_string(GetDevID());

    while (1)
    {
        OSTaskDelay(3 * 1000);

    	Json::Value jsonData = Json::objectValue;
        Json::Value jsonInfo = Json::objectValue;

        //get video info

        WORD wVideoWidth = 0;
        FpgaReadReg(SPI_REGADDR_VIDEO_WIDTH, &wVideoWidth);

        WORD wVideoHeight = 0;
        FpgaReadReg(SPI_REGADDR_VIDEO_HIGH, &wVideoHeight);

        WORD wVideoStatus = 0;
        WORD wVideoRefreshRate = 0;
        FpgaReadReg(SPI_REGADDR_VIDEO_STATUS, &wVideoStatus);
        wVideoRefreshRate = wVideoStatus & 0xff;
        wVideoStatus = (wVideoStatus >> 15) & 0x01;

        bool isException = true;
        WORD wHdmiStat = ThriftCommon::ThriftCall<Gsv2002ServerClient>(isException, m_Gsv2002Client, &Gsv2002ServerClient::GetHdmiStatus);
        
        if(bFirst || m_notifyQtUi ||
                (m_wVideoStat != wVideoStatus || (m_wHdmiStat != wHdmiStat && !isException) ||
                 wVideoWidth != RxInfo_st.wWidth || wVideoHeight != RxInfo_st.wHeight || wVideoRefreshRate != RxInfo_st.wRefreshRate))
        {
            if(m_wVideoStat != wVideoStatus)
                m_wVideoStat = wVideoStatus;
            if(m_wHdmiStat != wHdmiStat)
                m_wHdmiStat = wHdmiStat;
            jsonInfo["status"]["video_valid"] = (m_wVideoStat && m_wHdmiStat) ? 1 : 0;

            if(m_notifyQtUi) {
                SetNetStat(GetNetStat());
                bool bError = false;
                ThriftCommon::ThriftCall<QtServerClient, int32_t, int32_t>
                        (bError, m_QtClient, &QtServerClient::SetOutputResolution, std::move(wVideoWidth), std::move(wVideoHeight));
                if(bError)
                {
                    PRINT_FAULT("QtServerClient::SetOutputResolution (%d,%d) failed!\n", wVideoWidth, wVideoHeight);
                }
                else
                {
                    m_notifyQtUi = false;
                }
            }

            if(wVideoWidth != RxInfo_st.wWidth)
                RxInfo_st.wWidth = wVideoWidth;
            if(wVideoHeight != RxInfo_st.wHeight)
                RxInfo_st.wHeight = wVideoHeight;
            if(wVideoRefreshRate != RxInfo_st.wRefreshRate)
                RxInfo_st.wRefreshRate = wVideoRefreshRate;

            jsonInfo["screen"]["width"] = RxInfo_st.wRefreshRate ? RxInfo_st.wWidth : 0;
            jsonInfo["screen"]["height"] = RxInfo_st.wRefreshRate ? RxInfo_st.wHeight : 0;
            jsonInfo["screen"]["refresh_rate"] = RxInfo_st.wRefreshRate;
        }
        
        //get temp status
        WORD wTemp = 0;
        FpgaReadReg(SPI_REGADDR_TEMP, &wTemp);
        wTemp = wTemp * 503.975 / 4096 - 273.15;
        if(bFirst || abs(wTemp - m_wTemp) >= 5)
        {
            m_wTemp = wTemp;
            jsonInfo["status"]["temp"] = m_wTemp;
        }

        if(bFirst && s32FirstCnt++ > 5)
            bFirst = false;

        if(!jsonInfo.empty())
        {
            jsonInfo["device_id"] = (Json::UInt64)GetDevID();
            jsonData["notify"].append(jsonInfo);
            jsonObject["data"] = jsonData;
            //std::cout<< "JsonReq:"<< JsonReq.toStyledString()<< std::endl;
            FpgaSpiManager::GetInstance()->SendJsonCmd(jsonObject);
        }
    }
}

VOID* FpgaSpiManager::TaskCheckStatusEntry(VOID * p)
{
    if (p == NULL)
    {
        return NULL;
    }
    FpgaSpiManager *pThis = (FpgaSpiManager *)p;
    pThis->TaskCheckStatus();
    return NULL;
}


SDWORD FpgaSpiManager::PushMsgToMsQ(BYTE* pbyData, const DWORD &dwDataLen)
{
// 把数据送到TaskProcess
    if(dwDataLen == 0 || pbyData == NULL)
    {
        PRINT_FAULT("Param error"); 
    }
    
	int  dwResult = SYS_OK;
    BYTE*  pBuffer = new BYTE[dwDataLen];
	if(NULL == pBuffer)
	{
		PRINT_FAULT("New error!");
		dwResult = SYS_ERROR;
	}

	if(SYS_OK == dwResult)
	{
		memcpy(pBuffer, pbyData, dwDataLen);
		dwResult = msgQSend(m_MsgQ, pBuffer, dwDataLen, NO_WAIT, FpgaSpi_MSQ_INPUT_DATA);
		//如果发送失败，那么释放内存
		if (SYS_ERROR == dwResult)
		{
            PRINT_FAULT("FpgaSpiManager::msgQSend ERROR! %s\n",strerror(errno));
			OSTaskDelay(100);

            PRINT_DEBUG("Clear FpgaSpi_MSQ_INPUT_DATA!");
            BYTE byTempBuffer[MAXBUFSIZE];
            do {
                dwResult = msgQReceive(m_MsgQ, (CHAR *)(byTempBuffer), MAXBUFSIZE, NO_WAIT, FpgaSpi_MSQ_INPUT_DATA);
            } while(dwResult != SYS_ERROR);
            PRINT_DEBUG("Clear FpgaSpi_MSQ_INPUT_DATA Done!");
		}
	}
	
	delete[] pBuffer;
	return dwResult;

}

/*
0       2         4        6       7      9        11    12    13      n 
--------+---------+--------+-------+------+---------+-----+-----+--------
CheckID |ProjectID| DataLen|Reserve| DataBody = Cmd+Session+Src+Dst+Data
0x4c42  | 0x076D  |  n-7   |   0   | Cmd  | Session | Src | Dev | Data
--------+---------+--------+-------+------+---------+-----+-----+--------
DataLen为DataBody的长度
Data的长度为DataLen-6
func:当前参数中的数据为Data ,Data的长度wBufLen，DataLen=wBufLen+6
*/
SDWORD FpgaSpiManager::PushResponeToMsQ(const ITCMsgHeader* pstMsqHeader,const BYTE* &byBuf,const WORD &wBufLen)
{
// 把数据送到TaskSendEntry,Adapter层使用此函数写数据回串口
    DWORD          dwResult = SYS_ERROR;    
    BYTE *pbyBuf = new BYTE[MAXBUFSIZE];

	// 把包头里面的源和目的调反
    ITCMsgHeader *pMsqHeader = (ITCMsgHeader*)pbyBuf;
    pMsqHeader->wSession = pstMsqHeader->wSession;
    pMsqHeader->byDstDev = pstMsqHeader->bySrcDev;
    pMsqHeader->bySrcDev = pstMsqHeader->byDstDev;
	pMsqHeader->wCheckId = pstMsqHeader->wCheckId;
	pMsqHeader->wProjectId = pstMsqHeader->wProjectId;
	pMsqHeader->wDataLen = wBufLen+6;
	pMsqHeader->byReserve = pstMsqHeader->byReserve;
	pMsqHeader->wCmdId = pstMsqHeader->wCmdId;
	
 	memcpy(pbyBuf+sizeof(ITCMsgHeader), byBuf, wBufLen);
	
	dwResult = msgQSend(m_MsgQ, pbyBuf, wBufLen+sizeof(ITCMsgHeader) ,NO_WAIT,FpgaSpi_MSQ_OUTPUT_DATA);
    //如果发送失败，那么释放内存
	if (SYS_OK != dwResult)
	{
  		PRINT_FAULT("FpgaSpiManager::msgQSend ERROR!");
	}

    delete[] pbyBuf;
   return dwResult; 
}

SDWORD FpgaSpiManager::ResponeSendCbEntry(void* pUserData,const BYTE* byBuf,WORD wBufLen,void * pThis)
{
    if(pThis != NULL)
    {
        FpgaSpiManager* p = (FpgaSpiManager*)pThis;
        p->PushResponeToMsQ((ITCMsgHeader*)pUserData, byBuf,wBufLen);
        return SYS_OK;
    }

    return SYS_ERROR;
}

VOID* FpgaSpiManager::TaskReadIntStatEntry(VOID *p)
{
    FpgaSpiManager::GetInstance()->TaskReadIntStat();
    return p;
}

VOID* FpgaSpiManager::TaskReadRegStatEntry(VOID *p)
{
    FpgaSpiManager::GetInstance()->TaskReadRegStat();
    return p;
}

VOID* FpgaSpiManager::TaskLinkStatEntry(VOID *p)
{
    FpgaSpiManager::GetInstance()->TaskLinkStat();
    return p;
}


VOID* FpgaSpiManager::TaskReadSpiDataEntry(VOID *p)
{
	if( p == NULL)
	{
		PRINT_FAULT("TaskReadSpiDataEntry ERROR\n");
		return NULL;
	}

	FPGA_LOCK_TYPE_DSC_S *stTypeDsc = (FPGA_LOCK_TYPE_DSC_S *)p;
	FpgaSpiManager *pThis = (FpgaSpiManager *)stTypeDsc->pThis;
	pThis->TaskRecv(stTypeDsc);

	return NULL;
}

#define FPGA_IRQ      "/sys/devices/platform/fe630000.spi/spi_master/spi2/spi2.0/fpga_irq"

void FpgaSpiManager::TaskRecv(FPGA_LOCK_TYPE_DSC_S *stTypeDsc)
{
    sleep(5);

	if( stTypeDsc == NULL)
	{
		PRINT_FAULT("TaskRecv pIntFlag==NULL,ERROR\n");
		return;
	}
	
	MouseMessageQueue::MouseMessageNodeDesc node;
	bzero(&node,sizeof(node));
	
	SDWORD sdwRes = SYS_ERROR;
    int i = 0, j = 0, count = 0;
    struct FPGA_USR_PACKET packet;
    ITCMsgHeader *pMsqHeader = (ITCMsgHeader *)packet.data;
    Inputevent_st *pGetInput = reinterpret_cast<Inputevent_st*>(packet.data+sizeof(FpgaSpiMsqHeader));
	node.dwDataLen = sizeof(Inputevent_st);

#define FPGA_SPI_POLL_NFDS  1
    struct pollfd *fds  = (struct pollfd *)malloc(sizeof(struct pollfd) * FPGA_SPI_POLL_NFDS);
    //初始化fd
    fds[0].fd = m_FpgaDevFd;
    fds[0].events = POLL_IN;

    while(1) {
        sdwRes = poll(fds, FPGA_SPI_POLL_NFDS, -1);
        if(sdwRes <= 0) {
            PRINT_FAULT("poll fail.");
            continue;
        }

        for( i = 0; i < FPGA_SPI_POLL_NFDS; i++) {

            //读取fpga数据
            if((fds[i].fd== m_FpgaDevFd) && (fds[i].revents & POLL_IN)) {

                sdwRes = ioctl(m_FpgaDevFd, FPGA_SPI_FIFO_UNPOP_COUNT_R, &count);
                 if (sdwRes == -1)
                     return ;

                 for( j = 0; j < count; j++){
                    sdwRes = FpgaReadPacket(&packet);

                    if(SYS_OK == sdwRes){
                         // 将数据传输到TaskProcess
                         switch (packet.type) {
                             case FPGA_TYPE_SPI_CMD1:
                             case FPGA_TYPE_SPI_CMD2:
                                    //PRINT_DEBUG("recv cmd.");
                                    //PRINT_DEBUG_BUFFER(packet.data, packet.data_length);
                                    if(pMsqHeader->byReserve != 0)
										PushMsgToMsQ((BYTE *)pMsqHeader, pMsqHeader->wDataLen+sizeof(ITCHeader)+6);
									else
                                    	PushMsgToMsQ((BYTE *)pMsqHeader, pMsqHeader->wDataLen+sizeof(ITCHeader));
                                 break;
                             case FPGA_TYPE_SPI_HID:
                             case FPGA_TYPE_SPI_KMR:
                                    //将数据传输到KvmManager
                                  MouseMessageQueue::MouseMessageNodeDesc node;
                                  node.dwDataLen = pMsqHeader->wDataLen+sizeof(ITCHeader);

                                  if(m_pKmrHidRecvQueue) {
                                      m_pKmrHidRecvQueue->PushNode(&node, (BYTE *)pMsqHeader);
                                      m_pKmrHidRecvQueue->Post();
                                  }
                                 break;
                             case FPGA_TYPE_SPI_PVI_BASE:
                             case FPGA_TYPE_SPI_PVI_BASE_EX:{
#if 0
                                static unsigned int test = 0;
                                printf("[%d]recv %d pvi data\n", test++, packet.data_length);

                                static FILE *fp = fopen("test_rx.txt", "w+");
                                if(fp)
                                {
                                    fwrite(packet.data+7+22, 1, packet.data_length-7-22, fp);
                                    fflush(fp);
                                }
                                
                                FpgaWrite(SPI_INT_PVI_SEND_OVER, (BYTE *)g_aPviAck, sizeof(g_aPviAck));
#else
                                gettimeofday(&m_stGetPviDataBegin, nullptr);
                                //send ack to mx after get pvi data 

                                if(m_byPviAckStatus) {
                                    FpgaWrite(SPI_INT_PVI_SEND_OVER, (BYTE *)g_aPviAck, sizeof(g_aPviAck));
                                }

                                //PRINT_DEBUG("recv pvi len:%d.\n", packet.data_length);
                                static BYTE buf[128] = {0};
                                ITCPviPacket * pviPacket = (ITCPviPacket*)(packet.data);

                                //PRINT_DEBUG("recv pvi frame len:%d, ts:%lu, num:%d.", pviPacket->wOneFrameLength, pviPacket->dwTimestamp, pviPacket->wPackNum);
                                int ret = msgQSend(m_PviMsgQId, packet.data, packet.data_length, NO_WAIT, PVI_MSQ_INPUT_DATA);
                                if(ret < 0) {
                                    //TODO:发送失败
                                    PRINT_FAULT("pvi msqsend fail, err:%d, %s.", ret, strerror(errno));
                                    //printf("pvi msqsend fail, err:%d, %s.", ret, strerror(errno));
                                }
#endif
                                break;
                             }

                             default:
                                PRINT_DEBUG("%s: default break\n", __func__);
                                 break;
                         }
                     }
                 }
            }

            //读取fpga中断
            if(fds[i].revents & POLLHUP) {
                int fd = open(FPGA_IRQ, O_RDWR);
                if(fd > 0) {
                    uint32_t irq_type = 0;
                   sdwRes = read(fd, &irq_type, sizeof(uint32_t));
                   if(sdwRes < 0) {
                       PRINT_FAULT("read fpga iqr fail.");
                       continue;
                   }
                   PRINT_FAULT("fpga irq:%#x.", irq_type);
                   g_FpgaInt = irq_type;
                   m_SemFpgaInt.signal();
                   close(fd);
                }  else {
                    PRINT_FAULT("irq [%s] open fail.", FPGA_IRQ);
                }
            }

        }
    }


				
    if(fds) free(fds);

}


void FpgaSpiManager::TaskLinkStat()
{
	
	static int LinkStatus = 0;
	DevID_t wDevId = GetDevID();
	while(1)
	{
		PRINT_DEBUG("LinkStatus:%d.", LinkStatus); 
		WORD wStatus;
        FpgaReadReg(SPI_REGADDR_STATUS, &wStatus);
        switch (wStatus & 0x3) {
        case 0x1:
            g_FirState = FIRB_VALID_1;
            break;
        case 0x2:
            g_FirState = FIRB_VALID_2;
            break;
        case 0x3:
            g_FirState = FIRB_VALID_ALL;
            break;
        default:
            g_FirState = FIRB_INVALID_ALL;
            break;
        }
		
		if(g_FirState != FIRB_INVALID_ALL && !LinkStatus)
		{
			Json::Value jsonReq;
			jsonReq["actioncode"] = "ReqQueryDevConnect";
			jsonReq["session"] = "会话";
			jsonReq["device_name"] = "Rx";
			jsonReq["data"]["user_id"] = -1;
			jsonReq["data"]["device_id"].append(wDevId);
			
			SendJsonCmd(jsonReq);
		}

		if((g_FirState != FIRB_VALID_1) && (g_FirState != FIRB_VALID_2) && (g_FirState != FIRB_VALID_ALL))
		{
			LinkStatus = 0;
		}
		else
		{
			LinkStatus = 1;
		}

		OSTaskDelay(1*1000);
	}
}

void FpgaSpiManager::TaskReadIntStat()
{
    sleep(5);

    while(1)
    {
        m_SemFpgaInt.wait();
        uint32_t FpgaInt = g_FpgaInt;

        //TODO:处理FPGA中断状态
        if(FpgaInt & SPI_INT_VIDEO_CHANGE_OVER) {
            PRINT_DEBUG("SPI_INT_VIDEO_CHANGE_OVER\n");
            m_OsdLocator->update();
        }

        if((FpgaInt & SPI_INT_LINK1_CHANGE_OVER) || (FpgaInt & SPI_INT_LINK2_CHANGE_OVER)) {
            if(FpgaInt & SPI_INT_LINK1_CHANGE_OVER)
                PRINT_DEBUG("SPI_INT_LINK1_CHANGE_OVER\n");
            if(FpgaInt & SPI_INT_LINK2_CHANGE_OVER)
                PRINT_DEBUG("SPI_INT_LINK2_CHANGE_OVER\n");

            WORD wStatus;
            FpgaReadReg(SPI_REGADDR_STATUS, &wStatus);
            switch (wStatus & 0x3) {
            case 0x1:
                g_FirState = FIRB_VALID_1;
                SetDevLocation2(0, 0);
                break;
            case 0x2:
                g_FirState = FIRB_VALID_2;
                SetDevLocation1(0, 0);
                break;
            case 0x3:
                g_FirState = FIRB_VALID_ALL;
                break;
            default:
                g_FirState = FIRB_INVALID_ALL;
                SetDevLocation1(0, 0);
                SetDevLocation2(0, 0);
                break;
            }
		#if 0
            WORD val = 0;
            FpgaReadReg(SPI_REGADDR_STATUS, &val);
            if(0 == (val&0x3)) {
                bool b;
                ThriftCommon::ThriftCall<KvmServerClient>
                        (b, m_KvmClient, &KvmServerClient::HidDisconnect);
                ThriftCommon::ThriftCall<KvmServerClient>
                        (b, m_KvmClient, &KvmServerClient::KmrDisconnect);
			
#if 0
                const std::string k = "NAME";
                ThriftCommon::ThriftCall<HardWareServerClient, const std::string &>
                        (b, m_HardWareClient, &HardWareServerClient::LedDelString, k);
#endif
            }
		#endif
        }

        if(FpgaInt & SPI_INT_TMDS_CHANGE_OVER) {
            PRINT_DEBUG("SPI_INT_TMDS_CHANGE_OVER\n");
            bool b;
            ThriftCommon::ThriftCall<HardWareServerClient>(b, m_HardWareClient, &HardWareServerClient::Sil9136SoftReset);
        }

		OSTaskDelay(100);
    }

}

void FpgaSpiManager::TaskReadRegStat()
{
    sleep(5);

    bool isException = false;
    //FpgaSpiAdapter *pFpgaSpiAdpter = pThis->m_pFpgaSpiAdpter;

    VERSION_ST versoft = {1,0,0};	// 后期改成从文件获取版本号(或者通过thrift从管理配置信息的进程获取)
    Heart_st HeartData1, HeartData2;
    WORD wHeartLen = sizeof(Heart_st)+6;

    BYTE byAllData[wHeartLen] = {0};
    ITCMsgHeader* pstParam = (ITCMsgHeader*)byAllData;
    Heart_st* pHeart = (Heart_st*)(byAllData + sizeof(ITCMsgHeader));

    pstParam->wCheckId = ITC_CHECK_ID;
    pstParam->wProjectId = ITC_PROJECT_ID;
    pstParam->wDataLen = sizeof(Heart_st);
    pstParam->byReserve = 0;
    pstParam->wCmdId = CMDID_HEART_DATA;
    pstParam->wSession = 0;
    pstParam->bySrcDev = DEVICE_TYPE_TV_6141RX;
    pstParam->byDstDev = DEVICE_TYPE_TV_ALL;

	DevID_t wDevId = GetDevID();
	HeartData1.ddwdevid = HeartData2.ddwdevid = wDevId;
	
    HeartData1.bychannel = 1;
    HeartData2.bychannel = 2;
    HeartData1.dwNum = HeartData2.dwNum = 1;
    HeartData1.versoft = HeartData2.versoft = versoft;
    //PRINT_TRACE("csj %d %d %d",HeartData1.versoft.byLargeVer, HeartData1.versoft.byLittleVer, HeartData1.versoft.bySmallVer);

    WORD wIntval = 0;
    WORD wGenIntMask = ~(    SPI_INT_CMD1_RECV_OVER | SPI_INT_CMD1_SEND_OVER | SPI_INT_CMD2_RECV_OVER | SPI_INT_CMD2_SEND_OVER
                        | SPI_INT_HID_RECV_OVER  | SPI_INT_HID_SEND_OVER
                        | SPI_INT_PVI_RECV_OVER  | SPI_INT_PVI_SEND_OVER
                        | SPI_INT_KMR_RECV_OVER  | SPI_INT_KMR_SEND_OVER);

	static int LinkStatus = 0;
	
    while(1)
    {
	    #if 0
        FpgaReadReg(SPI_INT_STATUS, &wIntval);

        wIntval &= wGenIntMask;

        //读取光纤连接状态
        WORD wStatus;
        FpgaReadReg(SPI_REGADDR_STATUS, &wStatus);
        switch (wStatus & 0x3) {
        case 0x1:
            g_FirState = FIRB_VALID_1;
            break;
        case 0x2:
            g_FirState = FIRB_VALID_2;
            break;
        case 0x3:
            g_FirState = FIRB_VALID_ALL;
            break;
        default:
            g_FirState = FIRB_INVALID_ALL;
            break;
        }
		#endif
        //PRINT_DEBUG("firstate:%d.", g_FirState);
		
        if(g_FirState != FIRB_INVALID_ALL)
        {
            if(g_FirState & FIRB_VALID_1)
            {
                HeartData1.ddwTime = GetUnixTimeInMsec();
                pHeart = &HeartData1;
				
                FpgaSpiManager::GetInstance()->SendCmd(CMDID_HEART_DATA, DEVICE_TYPE_TV_ALL,
                                    (BYTE *)pHeart, sizeof(Heart_st) , (int)FIRB_VALID_1);
				HeartData1.dwNum++;
            } else {
                FpgaSpiManager::GetInstance()->SetDevLocation1(0, 0);
            }

            if(g_FirState & FIRB_VALID_2)
            {
                HeartData2.ddwTime = GetUnixTimeInMsec();
                pHeart = &HeartData2;
				
                FpgaSpiManager::GetInstance()->SendCmd(CMDID_HEART_DATA, DEVICE_TYPE_TV_ALL,
                                    (BYTE *)pHeart, sizeof(Heart_st) , (int)FIRB_VALID_2);
				HeartData2.dwNum++;
            }else {
                FpgaSpiManager::GetInstance()->SetDevLocation2(0, 0);
            }

#if 1 // osd用于状态栏显示
            BYTE bySlot;
            BYTE byChannel;
            BYTE byReSlot;
            BYTE byReChannel;
            DevID_t wDevId;
			//wDevId = ReadCurDevIdFromFile();
            wDevId = GetDevID();
            FpgaSpiManager::GetInstance()->GetDevLocation1(&bySlot, &byChannel);
            FpgaSpiManager::GetInstance()->GetDevLocation2(&byReSlot, &byReChannel);

            CmdUnconfDevIdReq *pReq = (CmdUnconfDevIdReq *)malloc(sizeof(CmdUnconfDevIdReq) + sizeof(CmdUnconfDevIdReqItem));
            if(!pReq) {
                OSTaskDelay(1500);
                continue;
            }

            memset(pReq, 0, sizeof(CmdUnconfDevIdReq) + sizeof(CmdUnconfDevIdReqItem));

            pReq->wNum = 1;
            CmdUnconfDevIdReqItem *pItem = &pReq->item[0];
            if(!byChannel && (g_FirState & FIRB_VALID_1)) {
                PRINT_DEBUG("report unconfig channel1.");
                pItem->byChannel = 0xFF;
                FpgaSpiManager::GetInstance()->SendCmd(CMDID_REPORT_UNCONF_DEV_ID, DEVICE_TYPE_TV_6180M,
                                (BYTE *)pReq, sizeof(CmdUnconfDevIdReq) + sizeof(CmdUnconfDevIdReqItem),
                                (int )FIRB_VALID_1);
#if 0
                static int tryTime1 = 0;
                if(tryTime1++ > 10)
                {
                    PRINT_DEBUG("report unconfig channel1, will restart FpgaSpi module.");
                    exit(1);
                }
#endif

            }
            if(!byReChannel && (g_FirState & FIRB_VALID_2)) {
                PRINT_DEBUG("report unconfig channel2.");
                if(0xFF == pItem->byChannel) pItem->byChannel = 0x0;
                pItem->byReChannel = 0xff;
                FpgaSpiManager::GetInstance()->SendCmd(CMDID_REPORT_UNCONF_DEV_ID, DEVICE_TYPE_TV_6180M,
                                (BYTE *)pReq, sizeof(CmdUnconfDevIdReq) + sizeof(CmdUnconfDevIdReqItem),
                                (int)FIRB_VALID_2);
#if 0
                static int tryTime2 = 0;
                if(tryTime2++ > 10)
                {
                    PRINT_DEBUG("report unconfig channel2, will restart FpgaSpi module.");
                    exit(1);
                }
#endif
            }
            free(pReq);
#endif
            // WORD bVideoStat, wVidoeStat = 0;
            // FpgaReadReg(SPI_REGADDR_VIDEO_STATUS, &wVidoeStat);
            // bVideoStat = (wVidoeStat >> 15) & 0x01 ? 1 : 0;

            // if(bVideoStat != GetVideoStat())
            // {
            //     SetVideoStat(bVideoStat);
            //     SendJsonVideoStat(bVideoStat);
            // }
        }

        OSTaskDelay(4*1000);
    }

    return ;
}

SDWORD FpgaSpiManager::ResolutionInit()
{
	int wResolutionMode;
	IniReadInt(RESOLUTION_CFGFILE,"Resolution","mode",&wResolutionMode);
	
	if(wResolutionMode > 1)
	{
		FpgaWriteReg(SPI_REGADDR_OUTPUT_RESOLUTION, 0x60);
		//if(access(RESOLUTION_CFGFILE, F_OK) == 0)
		//{
			int wPixelclock16bit,wPixelclock3bit;
			int wHFrontPorch,wHBackPorch,wVFrontPorch,wVBackPorch;
			int wHSync,wVSync,wHActive, wVActive;
			IniReadInt(RESOLUTION_CFGFILE,"Resolution","pixel_clock_16bit",&wPixelclock16bit);
			IniReadInt(RESOLUTION_CFGFILE,"Resolution","pixel_clock_3bit",&wPixelclock3bit);
			IniReadInt(RESOLUTION_CFGFILE,"Resolution","h_front_porch",&wHFrontPorch);
			IniReadInt(RESOLUTION_CFGFILE,"Resolution","h_sync",&wHSync);
			IniReadInt(RESOLUTION_CFGFILE,"Resolution","h_back_porch",&wHBackPorch);
			IniReadInt(RESOLUTION_CFGFILE,"Resolution","h_active",&wHActive);
			IniReadInt(RESOLUTION_CFGFILE,"Resolution","v_front_porch",&wVFrontPorch);
			IniReadInt(RESOLUTION_CFGFILE,"Resolution","v_sync",&wVSync);
			IniReadInt(RESOLUTION_CFGFILE,"Resolution","v_back_porch",&wVBackPorch);
			IniReadInt(RESOLUTION_CFGFILE,"Resolution","v_active",&wVActive);

			FpgaWriteReg(SPI_REGADDR_OUTPUT_RESOLUTION_TMDS_LOW, wPixelclock16bit);
			FpgaWriteReg(SPI_REGADDR_OUTPUT_RESOLUTION_TMDS_HIGH, wPixelclock3bit);
			FpgaWriteReg(SPI_REGADDR_OUTPUT_H_FRONT_PORCH, wHFrontPorch);
			FpgaWriteReg(SPI_REGADDR_OUTPUT_H_SYNC, wHSync);
			FpgaWriteReg(SPI_REGADDR_OUTPUT_H_BACK_PORCH, wHBackPorch);
			FpgaWriteReg(SPI_REGADDR_OUTPUT_H_ACTIVE, wHActive);
			FpgaWriteReg(SPI_REGADDR_OUTPUT_V_FRONT_PORCH, wVFrontPorch);
			FpgaWriteReg(SPI_REGADDR_OUTPUT_V_SYNC, wVSync);
			FpgaWriteReg(SPI_REGADDR_OUTPUT_V_BACK_PORCH, wVBackPorch);
			FpgaWriteReg(SPI_REGADDR_OUTPUT_V_ACTIVE, wVActive);

			FpgaWriteReg(SPI_REGADDR_OUTPUT_RESOLUTION_SYNC, 1);
		//}
		
	}
	else
	{
		FpgaWriteReg(SPI_REGADDR_OUTPUT_RESOLUTION, 0x00);
	}
}

SDWORD FpgaSpiManager::UsbInit(bool bSwitch)
{
	Json::Value jsonTemp;
	
	if(bSwitch)
	{
		system("insmod /nand/ko/usbip-host.ko; rst_tool usb");
		jsonTemp["usb_status"] = 1;
	}
	else
    {
        system("rmmod usbip_host");
		jsonTemp["usb_status"] = 0;
    }
	SaveRxInfo(jsonTemp);
	
	return SYS_OK;
}

void FpgaSpiManager::OnAliveUp()
{
    OSTaskDelay(500);

    // 上电马上获取Rx信息
    memset(&RxInfo_st, 0, sizeof(JsonRxInfo));
    Json::Value JsonReq;
    Json::Value JsonReqTmp;

    JsonReq["actioncode"] = "ReqQueryRxData";
    JsonReq["session"] = "";
    JsonReq["device_name"] = "MX";

    JsonReqTmp["device_id"].append((Json::UInt64)GetDevID());
    JsonReqTmp["key"].append("screen");
    JsonReqTmp["key"].append("status");

    JsonReq["data"] = JsonReqTmp;

    //std::cout<< "JsonReq:"<< JsonReq.toStyledString()<< std::endl;
    FpgaSpiManager::GetInstance()->SendJsonCmd(JsonReq);

    OSTaskDelay(500);

    // 请求连接的Tx设备
    JsonReq.clear();
    JsonReqTmp.clear();

    JsonReq["actioncode"] = "ReqQueryDevConnect";
    JsonReq["session"] = "";
    JsonReq["device_name"] = "MX";

    JsonReqTmp["user_id"] = 0;
    JsonReqTmp["device_id"].append((Json::UInt64)GetDevID());

    JsonReq["data"] = JsonReqTmp;

    //std::cout<< "ReqQueryDevConnect:"<< JsonReq.toStyledString()<< std::endl;
    FpgaSpiManager::GetInstance()->SendJsonCmd(JsonReq);

    if(m_QtClient)
    {
        bool bException;
        ThriftCommon::ThriftCall<QtServerClient>
                (bException, m_QtClient, &QtServerClient::AliveUp);
        if(bException)
            PRINT_FAULT("KmrConnect call AliveUp failed!\n");
        else
            PRINT_DEBUG("KmrConnect call AliveUp successed!\n");

        ThriftCommon::ThriftCall<QtServerClient, bool>
                (bException, m_QtClient, &QtServerClient::SetCurRxStatus, FALSE);
        if(bException)
            PRINT_FAULT("KmrConnect call SetCurRxStatus FALSE failed!\n");
        else
            PRINT_DEBUG("KmrConnect call SetCurRxStatus FALSE successed!\n");
    }
}


SDWORD FpgaSpiManager::Init()
{
    m_MsgQ = msgQGet(FpgaSpi_MSQ_QUEUE_PATH, FpgaSpi_MSQ_QUEUE_ID);
	if(SYS_ERROR == m_MsgQ)
	{
		PRINT_FAULT("FpgaSpiManager::Init msgQInit ERROR!\n");
        return SYS_ERROR;
	}

    //pvi通信队列
    m_PviMsgQId = msgQGet(PVI_MSQ_QUEUE_PATH, PVI_MSQ_QUEUE_ID);
    if(SYS_ERROR == m_PviMsgQId) {
        PRINT_FAULT("pvi msq create fail.");
        return SYS_ERROR;
    }
    m_FpgaDevFd = FpgaInit();
	ResolutionInit();

	AdapterAbstract::ResponeToClient pFun = &FpgaSpiManager::ResponeSendCbEntry;
    m_pFpgaSpiCmdAdpter->Init(pFun,this);

    m_pMouseMessageQueue =  MouseMessageQueue::Attach(FRAME_QUEUE_MOUSE_QUEUE_ID_START);
    m_pKmrHidRecvQueue   =  MouseMessageQueue::Attach(FRAME_QUEUE_MOUSE_QUEUE_ID_KMR_HID_RECV);
    
	OS_TASK_DSC_S m_stTaskIntStat,m_stTaskFpgaStat,m_stTaskRecvcmd1,m_stTaskRecvpvi,
					m_stTaskRecvcmd2,m_stTaskRecvhid,m_stTaskRecvkmr, m_stTaskLinkStat;

	FPGA_PRO_DSC_S *psFpgaProLock = new FPGA_PRO_DSC_S;
	int swRet = -1;
	
	pthread_mutexattr_t mutex_attr;
	pthread_mutexattr_init(&mutex_attr);
	
	pthread_mutex_init(&psFpgaProLock->stLock, &mutex_attr);
	swRet  = sem_init(&psFpgaProLock->stSemCmd1, 0, 0);
	swRet |= sem_init(&psFpgaProLock->stSemCmd2, 0, 0);
	swRet |= sem_init(&psFpgaProLock->stSemHid, 0, 0);
	swRet |= sem_init(&psFpgaProLock->stSemKmr, 0, 0);
	swRet |= sem_init(&psFpgaProLock->stSemPvi, 0, 0);
	
	pthread_mutexattr_destroy(&mutex_attr);
	
	FPGA_LOCK_TYPE_DSC_S *psFpgaTypeCmd1 = new FPGA_LOCK_TYPE_DSC_S;
	psFpgaTypeCmd1->stLock = new pthread_mutex_t;
	psFpgaTypeCmd1->stSem = new sem_t;
	
	FPGA_LOCK_TYPE_DSC_S *psFpgaTypeCmd2 = new FPGA_LOCK_TYPE_DSC_S;
	psFpgaTypeCmd2->stLock = new pthread_mutex_t;
	psFpgaTypeCmd2->stSem = new sem_t;
	
	FPGA_LOCK_TYPE_DSC_S *psFpgaTypeHid = new FPGA_LOCK_TYPE_DSC_S;
	psFpgaTypeHid->stLock = new pthread_mutex_t;
	psFpgaTypeHid->stSem = new sem_t;
	
	FPGA_LOCK_TYPE_DSC_S *psFpgaTypeKmr = new FPGA_LOCK_TYPE_DSC_S;
	psFpgaTypeKmr->stLock = new pthread_mutex_t;
	psFpgaTypeKmr->stSem = new sem_t;
	
	FPGA_LOCK_TYPE_DSC_S *psFpgaTypePvi = new FPGA_LOCK_TYPE_DSC_S;
	psFpgaTypePvi->stLock = new pthread_mutex_t;
	psFpgaTypePvi->stSem = new sem_t;
	
	psFpgaTypeCmd1->SpiIntFlag = SPI_INT_CMD1_RECV_OVER;
	psFpgaTypeCmd2->SpiIntFlag = SPI_INT_CMD2_RECV_OVER;
	psFpgaTypeHid->SpiIntFlag = SPI_INT_HID_RECV_OVER;
	psFpgaTypeKmr->SpiIntFlag = SPI_INT_KMR_RECV_OVER;
	psFpgaTypePvi->SpiIntFlag = SPI_INT_PVI_RECV_OVER;
	
	// 必须用指针进行传递
	psFpgaTypeCmd1->stLock = &psFpgaProLock->stLock;
	psFpgaTypeCmd2->stLock = &psFpgaProLock->stLock;
	psFpgaTypeHid->stLock = &psFpgaProLock->stLock;
	psFpgaTypeKmr->stLock = &psFpgaProLock->stLock;
	psFpgaTypePvi->stLock = &psFpgaProLock->stLock;
	
	psFpgaTypeCmd1->stSem = &psFpgaProLock->stSemCmd1;
	psFpgaTypeCmd2->stSem = &psFpgaProLock->stSemCmd2;
	psFpgaTypeHid->stSem = &psFpgaProLock->stSemHid;
	psFpgaTypeKmr->stSem = &psFpgaProLock->stSemKmr;
	psFpgaTypePvi->stSem = &psFpgaProLock->stSemPvi;

	psFpgaTypeCmd1->pThis = this;
	psFpgaTypeCmd2->pThis = this;
	psFpgaTypeHid->pThis = this;
	psFpgaTypeKmr->pThis = this;
	psFpgaTypePvi->pThis = this;

	// 光纤口状态线程
	m_stTaskLinkStat.pTaskEntryFunc = TaskLinkStatEntry;
    m_stTaskLinkStat.dwTaskPri = OS_TASK_PRI_MIN;
    m_stTaskLinkStat.dwStackSize = OS_TASK_STACK_SIZE_DEFAULT;
    m_stTaskLinkStat.dwDetached = SYS_TRUE;
    m_stTaskLinkStat.pvParam = this;
    if(SYS_ERROR == OSTaskCreate(&m_stTaskLinkStat))
    {
        PRINT_FAULT("m_stTaskLinkStat oh my god!!!\n");
        return SYS_ERROR;
    }

#if 0

	// 读FPGA中断状态，此线程为所有业务的核心
	m_stTaskIntStat.pTaskEntryFunc = TaskReadIntStat;
	m_stTaskIntStat.dwTaskPri = OS_TASK_PRI_MAX;
	m_stTaskIntStat.dwStackSize = OS_TASK_STACK_SIZE_DEFAULT;
	m_stTaskIntStat.dwDetached = SYS_TRUE;
	m_stTaskIntStat.pvParam = psFpgaProLock;
	if(SYS_ERROR == OSTaskCreate(&m_stTaskIntStat))
	{
		PRINT_FAULT("TaskReadIntStat oh my god!!!\n");
		return SYS_ERROR;
	}


	// 读取CMD2
	m_stTaskRecvcmd2.pTaskEntryFunc = TaskReadSpiDataEntry;
	m_stTaskRecvcmd2.dwTaskPri = OS_TASK_PRI_MAX;
	m_stTaskRecvcmd2.dwStackSize = OS_TASK_STACK_SIZE_DEFAULT;
	m_stTaskRecvcmd2.dwDetached = SYS_TRUE;
	m_stTaskRecvcmd2.pvParam = psFpgaTypeCmd2;
	if(SYS_ERROR == OSTaskCreate(&m_stTaskRecvcmd2))
	{
		PRINT_FAULT("FPGAReadCMD2 oh my god!!!\n");
		return SYS_ERROR;
	}

	// 读取HID
	m_stTaskRecvhid.pTaskEntryFunc = TaskReadSpiDataEntry;
	m_stTaskRecvhid.dwTaskPri = OS_TASK_PRI_MAX;
	m_stTaskRecvhid.dwStackSize = OS_TASK_STACK_SIZE_DEFAULT;
	m_stTaskRecvhid.dwDetached = SYS_TRUE;
	m_stTaskRecvhid.pvParam = psFpgaTypeHid;
	if(SYS_ERROR == OSTaskCreate(&m_stTaskRecvhid))
	{
		PRINT_FAULT("FPGAReadHID oh my god!!!\n");
		return SYS_ERROR;
	}

	// 读取kmr
	m_stTaskRecvkmr.pTaskEntryFunc = TaskReadSpiDataEntry;
	m_stTaskRecvkmr.dwTaskPri = OS_TASK_PRI_MAX;
	m_stTaskRecvkmr.dwStackSize = OS_TASK_STACK_SIZE_DEFAULT;
	m_stTaskRecvkmr.dwDetached = SYS_TRUE;
	m_stTaskRecvkmr.pvParam = psFpgaTypeKmr;
	if(SYS_ERROR == OSTaskCreate(&m_stTaskRecvkmr))
	{
		PRINT_FAULT("FPGAReadKMR oh my god!!!\n");
		return SYS_ERROR;
	}

    // 读取SPI PVI,RX BOX专用
    m_stTaskRecvpvi.pTaskEntryFunc = TaskReadSpiDataEntry;
    m_stTaskRecvpvi.dwTaskPri = OS_TASK_PRI_MAX;
    m_stTaskRecvpvi.dwStackSize = OS_TASK_STACK_SIZE_DEFAULT;
    m_stTaskRecvpvi.dwDetached = SYS_TRUE;
    m_stTaskRecvpvi.pvParam = psFpgaTypePvi;
    if(SYS_ERROR == OSTaskCreate(&m_stTaskRecvpvi))
    {
        PRINT_FAULT("FPGAReadPVI oh my god!!!\n");
        return SYS_ERROR;
    }
#endif

    // 读取CMD1
    m_stTaskRecvcmd1.pTaskEntryFunc = TaskReadSpiDataEntry;
    m_stTaskRecvcmd1.dwTaskPri = OS_TASK_PRI_MAX;
    m_stTaskRecvcmd1.dwStackSize = OS_TASK_STACK_SIZE_DEFAULT;
    m_stTaskRecvcmd1.dwDetached = SYS_TRUE;
    m_stTaskRecvcmd1.pvParam = psFpgaTypeCmd1;
    if(SYS_ERROR == OSTaskCreate(&m_stTaskRecvcmd1))
    {
        PRINT_FAULT("FPGAReadCMD1 oh my god!!!\n");
        return SYS_ERROR;
    }

    // 1、清除状态 2、读取光口有效状态,用于判断数据发往哪个光口,3、发送定时心跳包
    // 不推荐这种写法 : 一个线程同时做3件事,除非这个线程做的事情高度单一
    m_stTaskFpgaStat.pTaskEntryFunc = TaskReadRegStatEntry;
    m_stTaskFpgaStat.dwTaskPri = OS_TASK_PRI_MIN;
    m_stTaskFpgaStat.dwStackSize = OS_TASK_STACK_SIZE_DEFAULT;
    m_stTaskFpgaStat.dwDetached = SYS_TRUE;
    m_stTaskFpgaStat.pvParam = this;
    if(SYS_ERROR == OSTaskCreate(&m_stTaskFpgaStat))
    {
        PRINT_FAULT("TaskReadFpgaStat oh my god!!!\n");
        return SYS_ERROR;
    }

	// CMD指令处理
	m_stTaskProcessDsc.pTaskEntryFunc = TaskProcessEntry;
    m_stTaskProcessDsc.dwTaskPri = OS_TASK_PRI_MAX;
    m_stTaskProcessDsc.dwStackSize = OS_TASK_STACK_SIZE_DEFAULT;
    m_stTaskProcessDsc.dwDetached = SYS_TRUE;
    m_stTaskProcessDsc.pvParam = this;
    if(SYS_ERROR == OSTaskCreate(&m_stTaskProcessDsc))
    {
        PRINT_FAULT("TaskProcessEntry oh my god!!!\n");
        return SYS_ERROR;
    }

    // CMD指令接收并送到FPGA spi
    m_stTaskSendDsc.pTaskEntryFunc = TaskSendEntry;
    m_stTaskSendDsc.dwTaskPri = OS_TASK_PRI_MAX;
    m_stTaskSendDsc.dwStackSize = OS_TASK_STACK_SIZE_DEFAULT;
    m_stTaskSendDsc.dwDetached = SYS_TRUE;
    m_stTaskSendDsc.pvParam = this;
    if(SYS_ERROR == OSTaskCreate(&m_stTaskSendDsc))
    {
        PRINT_FAULT("TaskSendEntry oh my god!!!\n");
        return SYS_ERROR;;
    }

    // 中断处理
    m_stTaskIntStat.pTaskEntryFunc = TaskReadIntStatEntry;
    m_stTaskIntStat.dwTaskPri = OS_TASK_PRI_MAX;
    m_stTaskIntStat.dwStackSize = OS_TASK_STACK_SIZE_DEFAULT;
    m_stTaskIntStat.dwDetached = SYS_TRUE;
    m_stTaskIntStat.pvParam = this;
    if(SYS_ERROR == OSTaskCreate(&m_stTaskIntStat))
    {
        PRINT_FAULT("TaskSendEntry oh my god!!!\n");
        return SYS_ERROR;;
    }

	// 键鼠数据共享内存接收(RX BOX特有form KvmManger)
	m_stTaskKvmGet.pTaskEntryFunc = TaskKvmGetEntry;
    m_stTaskKvmGet.dwTaskPri = OS_TASK_PRI_MAX;
    m_stTaskKvmGet.dwStackSize = OS_TASK_STACK_SIZE_DEFAULT;
    m_stTaskKvmGet.dwDetached = SYS_TRUE;
    m_stTaskKvmGet.pvParam = this;
    if(SYS_ERROR == OSTaskCreate(&m_stTaskKvmGet))
    {
        PRINT_FAULT("TaskKvmGetEntry oh my god!!!\n");
        return SYS_ERROR;;
    }

    m_stTaskPviAckDsc.pTaskEntryFunc = TaskPviAckEntry;
    m_stTaskPviAckDsc.dwTaskPri = OS_TASK_PRI_MAX;
    m_stTaskPviAckDsc.dwStackSize = OS_TASK_STACK_SIZE_DEFAULT;
    m_stTaskPviAckDsc.dwDetached = SYS_TRUE;
    m_stTaskPviAckDsc.pvParam = this;
    if(SYS_ERROR == OSTaskCreate(&m_stTaskPviAckDsc))
    {
        PRINT_FAULT("TaskPviAckEntry oh my god!!!\n");
        return SYS_ERROR;;
    }

    m_stTaskCheckStatusDsc.pTaskEntryFunc = TaskCheckStatusEntry;
    m_stTaskCheckStatusDsc.dwTaskPri = OS_TASK_PRI_MAX;
    m_stTaskCheckStatusDsc.dwStackSize = OS_TASK_STACK_SIZE_DEFAULT;
    m_stTaskCheckStatusDsc.dwDetached = SYS_TRUE;
    m_stTaskCheckStatusDsc.pvParam = this;
    if(SYS_ERROR == OSTaskCreate(&m_stTaskCheckStatusDsc))
    {
        PRINT_FAULT("TaskCheckStatusEntry oh my god!!!\n");
        return SYS_ERROR;;
    }

    if(!IniReadInt(OSDLOCATEINFO_INI, "LocateInfo", "LocatePolicy", (int *)&m_osdLocatePolicy)) {
		
		struct stat buf;
		stat(OSDLOCATEINFO_INI, &buf);
		if(buf.st_size == 0){
		PRINT_DEBUG("read file %s size is 0",OSDLOCATEINFO_INI);
		// 规避可能出现的板卡信息文件长度为0的问题
		remove(OSDLOCATEINFO_INI);
		}
		
        IniWriteInt(OSDLOCATEINFO_INI, "LocateInfo", "LocatePolicy", m_osdLocatePolicy);
    }

    m_OsdLocator = new OsdLocator();
    SetOsdLocatePolicy(m_osdLocatePolicy);

	InitThrift();

    return SYS_OK;
}

FpgaSpiManager * FpgaSpiManager::GetInstance()
{
    if (NULL == m_pInstance)
    {
        m_pInstance = new FpgaSpiManager();
		ProcessPriority(1);
    }

    return m_pInstance;
}

SDWORD FpgaSpiManager::SendCmd(WORD wCmdId, BYTE byDstDev, BYTE *pbyBuf, WORD wLength, BYTE byFir)
{
	static Common::Mutex mutex;

	Common::AutoLock l(&mutex);

	SDWORD sdwRet = SYS_ERROR;

	ITCMsgHeader *pstHeader = (ITCMsgHeader *)malloc(wLength + sizeof(ITCMsgHeader));
	if(!pstHeader) {
		return sdwRet;
	}
	memset(pstHeader, 0, sizeof(wLength + sizeof(ITCMsgHeader)));

    pstHeader->wCheckId = ITC_CHECK_ID;
	pstHeader->wProjectId = ITC_PROJECT_ID;
	pstHeader->wDataLen = sizeof(ITCMsgHeader) - 7 + wLength;
	pstHeader->byReserve = 0;
	pstHeader->wCmdId = wCmdId;
	pstHeader->wSession = 0;
	pstHeader->bySrcDev = DEVICE_TYPE_TV_6141RX;
	pstHeader->byDstDev = byDstDev;

	memcpy(pstHeader + 1, pbyBuf, wLength);

    //PRINT_DEBUG_BUFFER((BYTE *)pstHeader, pstHeader->wDataLen + sizeof(ITCHeader));
    sdwRet = SendCmdData((BYTE*)pstHeader, pstHeader->wDataLen + sizeof(ITCHeader), byFir);

	free(pstHeader);
	return sdwRet;
}
#include "IntervalLimitTrigger.h"
SDWORD FpgaSpiManager::SendCmdData(BYTE *pbyBuf, WORD wLength, BYTE byFir)
{
	SDWORD sdwRet = SYS_ERROR;
#if 1
    static IntervalLimitTrigger trigger(20);
    while(!trigger.trigger())
    {
        usleep(3*1000);
    }
#endif
	switch(byFir) {
        case FIRB_VALID_ALL: {
			if(g_FirState != FIRB_INVALID_ALL)
			{
				if(g_FirState != FIRB_VALID_2)
				{
                    sdwRet = SendFpgaData(SPI_INT_CMD1_SEND_OVER, pbyBuf,wLength);
				}
				else
				{
                    sdwRet = SendFpgaData(SPI_INT_CMD2_SEND_OVER, pbyBuf,wLength);
				}
			}
			break;
		}
		case FIRB_VALID_1:{
			if(g_FirState & FIRB_VALID_1) 
                sdwRet = SendFpgaData(SPI_INT_CMD1_SEND_OVER, pbyBuf,wLength);
			break;
		}
		case FIRB_VALID_2:{
			if(g_FirState & FIRB_VALID_2) 
                sdwRet = SendFpgaData(SPI_INT_CMD2_SEND_OVER, pbyBuf,wLength);
			break;
		}
		default:
            PRINT_FAULT("firstate err:%d.", byFir);
			break;
	}
    if(sdwRet != SYS_ERROR)  {
        //PRINT_DEBUG_BUFFER(pbyBuf, wLength);
    }
    return sdwRet;
}

SDWORD FpgaSpiManager::SendPviData(BYTE *pbyBuf, WORD wLength)
{
    return SendFpgaData(SPI_INT_PVI_SEND_OVER, pbyBuf, wLength);
}

SDWORD FpgaSpiManager::SendKmrData(BYTE *pbyBuf, WORD wLength)
{
    return SendFpgaData(SPI_INT_KMR_SEND_OVER, pbyBuf, wLength);
}

SDWORD FpgaSpiManager::SendHidData(BYTE *pbyBuf, WORD wLength)
{
    //PRINT_DEBUG_BUFFER(pbyBuf, wLength);
    return SendFpgaData(SPI_INT_HID_SEND_OVER, pbyBuf, wLength);
}

/*
0       2         4        6       7      9        11    12    13      n
--------+---------+--------+-------+------+---------+-----+-----+--------
CheckID |ProjectID| DataLen|Reserve| DataBody = Cmd+Session+Src+Dst+Data
0x424C  | 0x076D  |  n-7   |   0   | Cmd  | Session | Src | Dev | Data
--------+---------+--------+-------+------+---------+-----+-----+--------
DataLen为DataBody的长度
Data的长度为DataLen-6
func:当前需要处理DataLen及后的所有数据，共 n-7+sizeof(DataLen+Reserve) 个字节
*/
SDWORD FpgaSpiManager::SendFpgaData(SPI_INT_FLAG_E eFlag, BYTE *byBuf, WORD wLength)
{
    //PRINT_DEBUG("send fpga data flag:%#x, len:%d, write len:%d.",(int)eFlag, wLength);
    //PRINT_DEBUG_BUFFER(byBuf, wLength);

    //根据驱动接口，不需要传头7个字节，头7个字节由驱动自动补充
    return FpgaWrite(eFlag, byBuf, wLength);
}

SDWORD FpgaSpiManager::SendJsonCmd(const Json::Value& jsonMsg, BYTE byFir)
{
	static Common::Mutex mutex;

	Common::AutoLock l(&mutex);

    PRINT_DEBUG("sned json(%d):%s", byFir, jsonMsg.toStyledString().c_str());
//	std::cout<< "send jsonMsg:"<< jsonMsg.toStyledString()<< std::endl;
	
	SDWORD sdwRet = SYS_ERROR;

	Json::FastWriter writer;
	Json::String str = writer.write(jsonMsg);
	WORD wDatalen = str.length();
	
	ITCHeader *pstHeader = (ITCHeader *)malloc(wDatalen + sizeof(ITCHeader));
	if(!pstHeader) {
		return sdwRet;
	}

	memset(pstHeader, 0, sizeof(wDatalen + sizeof(ITCHeader)));

    pstHeader->wCheckId = ITC_CHECK_ID;
	pstHeader->wProjectId = ITC_JSON_PROJECT_ID;
	pstHeader->wDataLen = wDatalen;
	pstHeader->byReserve = 0;

	memcpy(pstHeader + 1, str.data(), pstHeader->wDataLen);


	//PRINT_DEBUG_BUFFER((BYTE *)pstHeader, pstHeader->wDataLen + sizeof(ITCHeader));
	sdwRet = SendCmdData((BYTE*)pstHeader, pstHeader->wDataLen + sizeof(ITCHeader), byFir);

	free(pstHeader);
	return sdwRet;
}

SDWORD FpgaSpiManager::SendJsonCmdToUi(const Json::Value& jsonMsg)
{
    bool bException;
	static Common::Mutex mutex;

	Common::AutoLock l(&mutex);

	SDWORD sdwRet = SYS_ERROR;

	Json::FastWriter writer;
	Json::String str = writer.write(jsonMsg);
    ThriftCommon::ThriftCall<QtServerClient, const string &>
        (bException, m_QtClient, &QtServerClient::SendJsonData, str);

	return bException ? SYS_OK : SYS_ERROR;
}

SDWORD FpgaSpiManager::SendJsonVideoStat(bool videoStat)
{
	SDWORD sdwRet = SYS_ERROR;

	Json::Value jsonObject;
	Json::Value jsonArray;
	Json::Value jsonTemp;

	DevID_t wDevId = GetDevID();

	jsonObject["actioncode"] = "NtfMxNotifyDeviceChange";
	jsonObject["session"] = "";
	jsonObject["device_name"] = "MX";

	jsonTemp["sw_version"] = "1.0.0";
	jsonTemp["model"] = "TV-6141RX";
	jsonTemp["sn"] = (Json::UInt64)wDevId;
	jsonTemp["type"] = "RX";
	jsonTemp["video_intf"] = "HDMI";
	jsonTemp["update_time"] = "2022-6-20 00:00";	// TODO:上次升级的时间
	
	jsonArray["identity"] = jsonTemp;

	jsonTemp.clear();
	jsonTemp["video_valid"] = videoStat;

	WORD wTemp;
	FpgaReadReg(SPI_REGADDR_TEMP, &wTemp);
	float fTemp = wTemp * 503.975 / 4096 - 273.15;
	int temp = (int)fTemp;
	jsonTemp["temp"] = temp;

	struct sysinfo sys_info;
	sysinfo(&sys_info);
	DWORD dwRunTime = (DWORD)sys_info.uptime;
	jsonTemp["run_time"] = dwRunTime;
	
	jsonArray["status"] = jsonTemp;	

	
    BYTE bySlot;
    BYTE byChannel;
    BYTE byReSlot;
    BYTE byReChannel;
    FpgaSpiManager::GetInstance()->GetDevLocation1(&bySlot, &byChannel);
    FpgaSpiManager::GetInstance()->GetDevLocation2(&byReSlot, &byReChannel);

    if(!byChannel && (g_FirState & FIRB_VALID_1)) {
		
		jsonTemp.clear();
		jsonTemp["name"] = FpgaSpiManager::GetInstance()->GetDevName();
		jsonTemp["alive"] = 1;
		jsonTemp["slot"] = 0;
		jsonTemp["port"] = 0xff;
		jsonTemp["re_alive"] = 0;
		jsonTemp["re_slot"] = 0;
		jsonTemp["re_port"] = 0;
		
		jsonArray["config"] = jsonTemp;
		
		jsonObject["data"]["notify"].append(jsonArray);

		//std::cout<< "SendJsonVideoStat:"<< jsonObject.toStyledString()<< std::endl;
		
		FpgaSpiManager::GetInstance()->SendJsonCmd(jsonObject, (int )FIRB_VALID_1);
    }
    if(!byReChannel && (g_FirState & FIRB_VALID_2)) {

		jsonTemp.clear();
		jsonTemp["name"] = FpgaSpiManager::GetInstance()->GetDevName();
		jsonTemp["alive"] = 0;
		jsonTemp["slot"] = 0;
		jsonTemp["port"] = 0;
		jsonTemp["re_alive"] = 1;
		jsonTemp["re_slot"] = 0;
		jsonTemp["re_port"] = 0xff;
		
		jsonArray["config"] = jsonTemp;
		
		jsonObject["data"]["notify"].append(jsonArray);
		
		//std::cout<< "SendJsonVideoStat:"<< jsonObject.toStyledString()<< std::endl;

		FpgaSpiManager::GetInstance()->SendJsonCmd(jsonObject, (int )FIRB_VALID_2);
    }

	return sdwRet;
}

DevID_t FpgaSpiManager::GetDevID()
{
#if 1
    static DevID_t id = ::GetDevID();
    if(id < 0)
        id = ::GetDevID();
    return id;
#else
  DevID_t ddwDevID;
  const char *cmd = R"(grep -m 1 'Serial' /proc/cpuinfo |cut -d " " -f2)";
  char* ret = nullptr;
  static char tmp_shell[32];
  FILE* fp;

  if(cmd != nullptr) {
    if ((fp = popen(cmd, "r")) == nullptr) {
      printf("popen error: %s\n", strerror(errno));
      ret = nullptr;
    } else {
      memset(tmp_shell, 0, sizeof(tmp_shell));
      while (fgets(tmp_shell, sizeof(tmp_shell), fp) != nullptr);
      if (tmp_shell[strlen(tmp_shell) - 1] == '\n')
        tmp_shell[strlen(tmp_shell) - 1] = '\0';
      pclose(fp);
      ret = tmp_shell;
    }
  }
  
  ddwDevID = strtoull(ret, NULL, 16);
  ddwDevID -= (ddwDevID%1000);
  return ddwDevID;
#endif
}


VOID FpgaSpiManager::SetDevLocation1( BYTE byslot, BYTE byChannel){
    if(!(m_bySlot1||m_byChannel1))
    {
        if(byslot&&byChannel) {
            //OnAliveUp();
        }
    }

    if(m_bySlot1!=byslot && m_byChannel1!=byChannel)
        PRINT_DEBUG("set locatino1:%d, %d.", byslot, byChannel);

    m_bySlot1=byslot; m_byChannel1=byChannel;

    bool bError = false;           
    ThriftCommon::ThriftCall<QtServerClient, int16_t, int16_t, int32_t>
        (bError, m_QtClient, &QtServerClient::LinkChange, std::move(byslot), std::move(byChannel), 1);
    if(bError)
    {
        PRINT_FAULT("QtServerClient::LinkChange 1-(%d,%d) failed!", byslot, byChannel);
    }
}

VOID FpgaSpiManager::SetDevLocation2( BYTE byslot, BYTE byChannel){
    if(!(m_bySlot2||m_byChannel2))
    {
        if(byslot&&byChannel) {
            //OnAliveUp();
        }
    }

    if(m_bySlot2!=byslot && m_byChannel2!=byChannel)
        PRINT_DEBUG("set locatino2:%d, %d.", byslot, byChannel);

    m_bySlot2=byslot; m_byChannel2=byChannel;
    
    bool bError = false;           
    ThriftCommon::ThriftCall<QtServerClient, int16_t, int16_t, int32_t>
        (bError, m_QtClient, &QtServerClient::LinkChange, std::move(byslot), std::move(byChannel), 2);
    if(bError)
    {
        PRINT_FAULT("QtServerClient::LinkChange 2-(%d,%d) failed!", byslot, byChannel);
    }
}

VOID FpgaSpiManager::SaveRxInfo(Json::Value &Json)
{
	if(Json.isMember("login_user"))
		RxInfo_st.wLoginUser = Json["login_user"].asUInt();
	
	if(Json.isMember("multi_screen"))
		RxInfo_st.wMultiScreen = Json["multi_screen"].asUInt();

	if(Json.isMember("is_main_screen"))
		RxInfo_st.wIsMainScreen = Json["is_main_screen"].asUInt();
	
	if(Json.isMember("relation_seat"))
			RxInfo_st.wRelationSeat = Json["relation_seat"].asUInt();

	if(Json.isMember("usb_status"))
		RxInfo_st.wUsbStatus = Json["usb_status"].asUInt();

	if(Json.isMember("out_resolution"))
		RxInfo_st.wOutResolution = Json["out_resolution"].asUInt();

	if(Json.isMember("audio_output_mode"))
		RxInfo_st.wAudioOutputMode = Json["audio_output_mode"].asUInt();

	if(Json.isMember("edid_mode"))
		RxInfo_st.wEdidMode = Json["edid_mode"].asUInt();	

	if(Json.isMember("screen"))
	{
		Json::Value JsonTmp;
		JsonTmp = Json["screen"];
		
		RxInfo_st.wRefreshRate = JsonTmp["refresh_rate"].asUInt();
		RxInfo_st.wWidth = RxInfo_st.wRefreshRate ? JsonTmp["width"].asUInt() : 0;
		RxInfo_st.wHeight = RxInfo_st.wRefreshRate ? JsonTmp["height"].asUInt() : 0;
	}

	if(Json.isMember("status"))
	{
		Json::Value JsonTmp;
		JsonTmp = Json["status"];
		
		RxInfo_st.wHid = JsonTmp["hid"].asUInt();
		RxInfo_st.wUsb = JsonTmp["usb"].asUInt();
	}
}

VOID FpgaSpiManager::GetRxInfo(JsonRxInfo *RxInfo)
{
	memcpy(RxInfo, &RxInfo_st, sizeof(JsonRxInfo));
}


DeviceStatus FpgaSpiManager::GetDeviceStatus()
{
    DeviceStatus deviceStatus{};

    deviceStatus.Link1.Slot = m_bySlot1;
    deviceStatus.Link1.Port = m_byChannel1;
    deviceStatus.Link1.Status = m_bySlot1 && m_byChannel1;
    deviceStatus.Link2.Slot = m_bySlot2;
    deviceStatus.Link2.Port = m_byChannel2;
    deviceStatus.Link2.Status = m_bySlot2 && m_byChannel2;
    
    return deviceStatus;
}

void FpgaSpiManager::SetOsdLocatePolicy(const OSD_LOCATE_POLICY_E::type policy)
{
    m_OsdLocator->setPolicy(policy);
    m_OsdLocator->update();

	struct stat buf;
	stat(OSDLOCATEINFO_INI, &buf);
	if(buf.st_size == 0){
	PRINT_DEBUG("read file %s size is 0",OSDLOCATEINFO_INI);
	// 规避可能出现的板卡信息文件长度为0的问题
	remove(OSDLOCATEINFO_INI);
	}
	
    IniWriteInt(OSDLOCATEINFO_INI, "LocateInfo", "LocatePolicy", policy);
}

void FpgaSpiManager::SetLauncherGeometry(int x, int y, int width, int height)
{
    m_OsdLocator->setLauncherGeometry(x, y, width, height);
}

void FpgaSpiManager::NotifyOutResolutionChanged()
{
    if(!m_notifyQtUi)
        m_notifyQtUi = true;
}

void FpgaSpiManager::NotifyNetStatChanged()
{
//    SetNetStat(GetNetStat());
}

SDWORD FpgaSpiManager::HandleSubPack(BYTE *pbyBuf, WORD wLength)
{
	static 	CmdSubData_S cmdSubData;
	PRINT_DEBUG_BUFFER(pbyBuf, wLength);
	CMD_SUB_PACKET *pstCmdSubPacket = (CMD_SUB_PACKET *)pbyBuf;
    memcpy(cmdSubData.byBuf + cmdSubData.wUsed, pstCmdSubPacket->byData, pstCmdSubPacket->wLength);
    cmdSubData.wUsed += pstCmdSubPacket->wLength;

    //一帧完整的数据接收完毕
    if(pstCmdSubPacket->byCurNo == pstCmdSubPacket->byTotalCount) {
        //判断接收长度是否正确
        if(cmdSubData.wUsed != pstCmdSubPacket->wTotalSize) {
            PRINT_FAULT("cmdSubData.wUsed: %d, pstCmdSubPacket->wTotalSize: %d\n", 
                    cmdSubData.wUsed, 
                    pstCmdSubPacket->wTotalSize);

            PRINT_FAULT("0x%04X, 0x%04X - > 0x%04X, 0x%04X\n", pstCmdSubPacket->wCheckID, pstCmdSubPacket->wProjectID, 
                    ITC_CHECK_ID, ITC_JSON_PROJECT_ID);
            printf("%s: recv %d data:", __func__, wLength);
            for(int j=0; j < wLength; j++) {
            if(j % 20 == 0) printf("\n[%02d]: ", j / 20);
            printf("%02X ", *((BYTE *)pstCmdSubPacket + j));
            }
            printf("\n");

            PRINT_FAULT("cmd sub recv data len err.");
            cmdSubData.wUsed = 0;
            memset(cmdSubData.byBuf, 0, CMD_MAX_SUB_SLICE);
            return SYS_ERROR;
        }

		BYTE byCmdData[CMD_MAX_SUB_SLICE];
		ITCHeader *pITCHeader = (ITCHeader *)byCmdData;
		pITCHeader->wCheckId = ITC_CHECK_ID;
		pITCHeader->wProjectId = ITC_JSON_PROJECT_ID;
		pITCHeader->wDataLen = cmdSubData.wUsed;
		pITCHeader->byReserve = 0;
		memcpy(byCmdData + sizeof(ITCHeader), cmdSubData.byBuf, cmdSubData.wUsed);
		
#if 0
		printf("%s %s %d Len:%d\n", __FILE__, __FUNCTION__, __LINE__, cmdSubData.wUsed + sizeof(ITCHeader));
		for(int i = 0; i < cmdSubData.wUsed + sizeof(ITCHeader); i++)
		{
			if(i%16 == 0)
				printf("\n");
			printf("%02x ", byCmdData[i]);
		}
		printf("\n");
#endif
		
       	m_pFpgaSpiCmdAdpter->JsonDispatchCall(byCmdData, pITCHeader->wDataLen + sizeof(ITCHeader));
		
		cmdSubData.wUsed = 0;
        memset(cmdSubData.byBuf, 0, CMD_MAX_SUB_SLICE);
    }	

	return SYS_OK;
}


void FpgaSpiManagerInit(int argc, char** argv)
{
    PRINT_DEBUG("FpgaSpiManager start\n");
	
    PDInitTrace(PD_DEFAULT_CFG_PATH,  argv[1]);
    PDTelnetFkmDevInit(argv[1]);

    FpgaSpiManager *pFpgaSpiManager = FpgaSpiManager::GetInstance();
    if(SYS_ERROR == pFpgaSpiManager->Init())
    {
        PRINT_FAULT("FpgaSpiManager Error\n");
        exit(1);
    }

    pause();
    delete pFpgaSpiManager;
}

SDWORD FpgaSpiManager::SendTestData(const string & data)
{
    bool b = true;
    int32_t ret = 0;

    Json::Value rspRoot = Json::objectValue;
    Json::Value rspData = Json::objectValue;

    Json::Value root = Json::objectValue;
    try{
        if(!m_jsonReader.parse(data.c_str(),
                           data.c_str() + data.length(),
                           root))
        {
            printf("json parse fail.\n");
            ret = -1;
        }
    }
    catch(...){
       ret = -1;
       printf("json parse error, %.*s.", data.c_str());
    }

    if(ret == 0)
    {
        Json::String actionName = root["actioncode"].asString();
        printf("***************************************************\n");
        printf("FpgaSpiManager::%s: process json cmd: %s\n", __func__, actionName.c_str());
        printf("***************************************************\n");

        rspRoot["data"]["Token"] = root["data"]["Token"];
        if(actionName == "UserLoginReq")
        {
            rspRoot["actioncode"] = "UserLoginRes";
            rspData["UserID"] = 2;
            rspData["UserName"] = root["data"]["UserName"];
            rspData["UserType"] = 1;
            rspData["UserLevel"] = 2;
            rspRoot["data"] = rspData;
            rspRoot["data"]["Token"] = "1321854874654548654654abc6546854165483251548246876546514321789765432168764321";
        }
        else if(actionName == "UserLogoutReq")
        {
            rspRoot["actioncode"] = "UserLogoutRes";
        }
        else if(actionName == "InputSignalSourceAllAreaRep")
        {
            rspRoot["actioncode"] = "InputSignalSourceAllAreaRsp";

            Json::Value areaArray = Json::arrayValue;

            Json::Value areaAll = Json::objectValue;
            areaAll["AreaId"] = 999;
            areaAll["AreaName"] = "全部";
            areaArray.append(areaAll);

            for(int i=0; i<3; i++)
            {
                Json::Value area = Json::objectValue;
                area["AreaId"] = i;
                area["AreaName"] = string("区域 ") + std::to_string(i+1);
                areaArray.append(area);
            }

            Json::Value areaOther = Json::objectValue;
            areaOther["AreaId"] = -1;
            areaOther["AreaName"] = "未分区";
            areaArray.append(areaOther);

            rspRoot["data"]["AreaArray"] = areaArray;
        }
        else if(actionName == "InputSignalSourceAreaGroupRep")
        {
            rspRoot["actioncode"] = "InputSignalSourceAreaGroupRsp";
            Json::Value groupArray = Json::arrayValue;
            int areaId = root["data"]["AreaId"].asInt();
            int limit = 0;

            Json::Value groupAll = Json::objectValue;
            groupAll["GroupId"] = 999;
            groupAll["GroupName"] = "全部";
            groupArray.append(groupAll);

            if(areaId == 999)
            {

                int id = 0;
                for(int i=0; i<0; i++)
                {
                    Json::Value group = Json::objectValue;
                    group["GroupId"] = id;
                    id++;
                    group["GroupName"] = string("A") + std::to_string(1) + string("G") + std::to_string(i+1);
                    groupArray.append(group);
                }
                for(int i=0; i<1; i++)
                {
                    Json::Value group = Json::objectValue;
                    group["GroupId"] = id;
                    id++;
                    group["GroupName"] = string("A") + std::to_string(2) + string("G") + std::to_string(i+1);
                    groupArray.append(group);
                }
                for(int i=0; i<2; i++)
                {
                    Json::Value group = Json::objectValue;
                    group["GroupId"] = id;
                    id++;
                    group["GroupName"] = string("A") + std::to_string(3) + string("G") + std::to_string(i+1);
                    groupArray.append(group);
                }
            }
            else
            {
                int groupId = 0;
                if(areaId == 0)
                    limit = 0;
                else if(areaId == 1)
                {
                    limit = 1;
                    groupId = 0;
                }
                else if(areaId == 2)
                {
                    limit = 2;
                    groupId = 1;
                }
                for(int i=0; i<limit; i++)
                {
                    Json::Value group = Json::objectValue;
                    group["GroupId"] = groupId+i;
                    group["GroupName"] = string("A") + std::to_string(areaId+1) + string("G") + std::to_string(i+1);
                    groupArray.append(group);
                }
            }

            Json::Value groupOther = Json::objectValue;
            groupOther["GroupId"] = -1;
            groupOther["GroupName"] = "未分组";
            groupArray.append(groupOther);

            rspRoot["data"]["AreaId"] = areaId;
            rspRoot["data"]["AreaName"] = string("区域 ") + std::to_string(root["data"]["AreaId"].asInt()+1);
            rspRoot["data"]["GroupArray"] = groupArray;
        }
        else if(actionName == "InputSignalSourceRep")
        {
            rspRoot["actioncode"] = "InputSignalSourceRes";
            Json::Value signalArray = Json::arrayValue;

            int areaId = root["data"]["AreaId"].asInt();
            int groupId = root["data"]["GroupId"].asInt();
            int online = 0;
            int offline = 0;

            for(int i=0; i<9; i++)
            {
                Json::Value signal = Json::objectValue;
                signal["Id"] = 4000+i;
                signal["SignalType"] = "TX_DEVICE";
                signal["Ip"] = "";
                signal["Mac"] = "";
                signal["SignalName"] = string("TX_") + std::to_string(4000+i);

                if(i == 3 || i == 4 || i == 5 || i == 7)
                    signal["Online"] = 0;
                else
                    signal["Online"] = 1;

                if(i == 1 || i == 5)
                    signal["Operation"] = 2;
                else
                    signal["Operation"] = 3;

                if(i == 3 || i == 4 || i == 6 || i == 7)
                {
                    signal["AreaId"] = 1;
                    signal["AreaName"] = "区域 2";
                    signal["GroupId"] = 0;
                    signal["GroupName"] = "A2G1";
                }
                if(i == 1 || i == 2 || i == 5 || i == 8)
                {
                    signal["AreaId"] = 2;
                    signal["AreaName"] = "区域 3";
                    signal["GroupId"] = 1;
                    signal["GroupName"] = "A3G1";
                }
                if(i == 0)
                {
                    signal["AreaId"] = 2;
                    signal["AreaName"] = "区域 3";
                    signal["GroupId"] = 2;
                    signal["GroupName"] = "A3G2";
                }
                if((areaId == 999 && groupId == 999)
                    || (areaId == 999 && groupId == signal["GroupId"].asInt())
                    || (areaId == signal["AreaId"].asInt() && groupId == 999)
                    || (areaId == signal["AreaId"].asInt() && groupId == signal["GroupId"].asInt()))
                {
                    if(signal["Online"].asInt())
                        online++;
                    else
                        offline++;
                    signalArray.append(signal);
                }
            }
            rspRoot["data"]["PageId"] = root["data"]["PageId"];
            rspRoot["data"]["EachPageMaxNum"] = root["data"]["EachPageMaxNum"];
            rspRoot["data"]["AreaId"] = root["data"]["AreaId"];
            rspRoot["data"]["GroupName"] = root["data"]["GroupName"];
            rspRoot["data"]["GroupId"] = root["data"]["GroupId"];
            rspRoot["data"]["SignalArray"] = signalArray;
            rspRoot["data"]["TotalNum"] = signalArray.size();
            rspRoot["data"]["OnlineNum"] = online;
            rspRoot["data"]["OfflineNum"] = offline;
        }
        else if(actionName == "AllRoamGroupRep")
        {
            rspRoot["actioncode"] = "AllRoamGroupRsp";
            Json::Value groupArray = Json::arrayValue;

            Json::Value groupAll = Json::objectValue;
            groupAll["GroupId"] = 999;
            groupAll["GroupName"] = "全部";
            groupArray.append(groupAll);

            for(int i=0; i<5; i++)
            {
                Json::Value group = Json::objectValue;
                group["GroupId"] = i;
                if(i != 0)
                    group["GroupName"] = string("坐席 ") + std::to_string(i);
                else
                    group["GroupName"] = string("大屏");
                groupArray.append(group);
            }

            Json::Value groupOther = Json::objectValue;
            groupOther["GroupId"] = -1;
            groupOther["GroupName"] = "未关联";
            groupArray.append(groupOther);

            rspRoot["data"]["SeatGroupArray"] = groupArray;
        }
        else if(actionName == "RoamGroupRep")
        {
            rspRoot["actioncode"] = "RoamGroupRes";
            Json::Value seatArray = Json::arrayValue;
            int groupId = root["data"]["GroupId"].asInt();
            int online = 0;
            int offline = 0;
            int tempId = 1;
            for(int i=0; i<9; i++)
            {
                Json::Value seat = Json::objectValue;

                seat["Id"] = 3000 + i;
                seat["Ip"] = "";
                seat["Mac"] = "";
                if(i == 5)
                {
                    seat["Attribute"] = "大屏";
                    seat["SeatName"] = "大鸟1屏";
                    seat["GroupId"] = 0;
                    seat["GroupName"] = "大屏";
                }
                else
                {
                    seat["Attribute"] = "坐席";
                    seat["SeatName"] = string("RX_") + std::to_string(3000+i);
                    seat["GroupId"] = tempId%4+1;
                    seat["GroupName"] = string("坐席 ") + std::to_string(tempId%4+1);
                    tempId++;
                }

                if(i == 3 || i == 4 || i == 7 || i == 8)
                    seat["Online"] = 0;
                else
                    seat["Online"] = 1;

                if(i == 1 || i == 5 || i == 6)
                    seat["Operation"] = 2;
                else
                    seat["Operation"] = 3;

               if((groupId == 999)
                   || (groupId == seat["GroupId"].asInt()))
               {
                    if(seat["Online"].asInt())
                        online++;
                    else
                        offline++;
                    seatArray.append(seat);
               }
            }
            rspRoot["data"]["PageId"] = root["data"]["PageId"];
            rspRoot["data"]["EachPageMaxNum"] = root["data"]["EachPageMaxNum"];
            rspRoot["data"]["GroupName"] = root["data"]["GroupName"];
            rspRoot["data"]["GroupId"] = root["data"]["GroupId"];
            rspRoot["data"]["SeatArray"] = seatArray;
            rspRoot["data"]["TotalNum"] = seatArray.size();
            rspRoot["data"]["OnlineNum"] = online;
            rspRoot["data"]["OfflineNum"] = offline;
        }
        else if(actionName == "ConnectReq")
        {
            rspRoot["actioncode"] = "ConnectRes";
            rspRoot["data"] = root["data"];
            rspRoot["data"]["TxName"] = string("TX_")+std::to_string(root["data"]["TxId"].asInt());
            rspRoot["data"]["RxName"] = string("RX_")+std::to_string(root["data"]["RxId"].asInt());
        }
        else if(actionName == "SceneListReq")
        {
            rspRoot["actioncode"] = "SceneListRes";
            int pageid = root["data"]["PageId"].asInt();
            int pageMaxNum = root["data"]["EachPageMaxNum"].asInt();
            Json::Value sceneArray = Json::arrayValue;
            for(int i=(pageid-1)*pageMaxNum; i<(pageid-1)*pageMaxNum + pageMaxNum && i < 23; i++)
            {
                Json::Value scene = Json::objectValue;
                scene["SceneId"] = i;
                scene["SceneName"] = string("场景 ") + std::to_string(i+1);
				if(i%2)
					scene["SplitMode"] = 1;
				else
					scene["SplitMode"] = 0;
                sceneArray.append(scene);
            }
            rspRoot["data"]["SceneArray"] = sceneArray;
            rspRoot["data"]["PageId"] = root["data"]["PageId"];
            rspRoot["data"]["EachPageMaxNum"] = root["data"]["EachPageMaxNum"];
            //rspRoot["data"]["TotalNum"] = sceneArray.size();
            rspRoot["data"]["TotalNum"] = 23;
        }
        else if(actionName == "CurrrentRxInfoRep")
        {
            rspRoot["actioncode"] = "CurrrentRxInfoRsp";
            rspData["CurrentGrpId"] = 1;
            rspData["CurrentGrpName"] = "坐席一";
            rspData["CurrentRxName"] = "RX_45bae15b76b37066";
            rspData["Mac"] = "";
            rspData["CurrentApplyScene"]["SceneId"] = 0;
            rspData["CurrentApplyScene"]["SceneName"] = "场景一";
            rspData["CurrentConnectSignal"]["ConnectTx"] = 0;
            rspData["CurrentConnectSignal"]["ConnectType"] = 0;
            rspData["Id"] = 5024576117234954342;
            rspData["Token"] = root["data"]["Token"];
            rspRoot["data"] = rspData;
            rspRoot["device_name"] = "MX_DEVICE";
            rspRoot["result"] = 200;
            rspRoot["return_message"] = "";
            Json::String str = m_jsonFastWriter.write(rspRoot);
            ThriftCommon::ThriftCall<QtServerClient, const string &>
                (b, m_QtClient, &QtServerClient::SendJsonData, str);
            return b;
        }
        else
            return false;
        rspRoot["data"]["Id"] = root["data"]["Id"];
        rspRoot["data"]["Platform"] = root["data"]["Platform"];
        rspRoot["device_name"] = "MX_DEVICE";
        rspRoot["result"] = 200;
        rspRoot["return_message"] = "";
        Json::String str = m_jsonFastWriter.write(rspRoot);
        printf("FpgaSpiManager::%s: send json cmd: [%d]%s\n", __func__, str.length(), rspRoot.toStyledString().c_str());
        ThriftCommon::ThriftCall<QtServerClient, const string &>
            (b, m_QtClient, &QtServerClient::SendJsonData, str);
    }
    return b;
}
