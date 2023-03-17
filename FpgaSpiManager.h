#ifndef __FpgaSpi_MANAGER_H__
#define __FpgaSpi_MANAGER_H__

#include <semaphore.h>
#include <sys/epoll.h>

#include "SysType.h"
#include "OsAbstract.h"
#include "ThriftCommon.hpp"
#include "AbstractManager.h"
#include "ini_wrapper.h"

#include "FpgaSpiMessage.h"
#include "FrameQueueRefactor.h"
#include "fpga.h"

#include "sem.h"
#include "UpdateManager.h"
#include "QtServer.h"
#include "UpdateServer.h"
#include "HardWareServer.h"
#include "Gsv2002Server.h"
#include "KvmServer.h"

#include <string.h>
#include "json.h"
#include "OsdLocator/osdlocator.h"
#include "Message.h"


#include <string>

using namespace std;

typedef struct _FPGA_POR_S
{
	pthread_mutex_t stLock;
	sem_t stSemCmd1;
	sem_t stSemCmd2;
	sem_t stSemHid;
	sem_t stSemKmr;
	sem_t stSemPvi;
}FPGA_PRO_DSC_S;

typedef struct _FPGA_LOCK_TYPE_S
{
	VOID *pThis;
	SPI_INT_FLAG_E SpiIntFlag;
	pthread_mutex_t *stLock;
	sem_t *stSem;
}FPGA_LOCK_TYPE_DSC_S;



class FpgaSpiCmdAdapter;

class FpgaSpiManager    :public AbstractManager
{
public:
	FpgaSpiManager();
	virtual ~FpgaSpiManager();
	
	static FpgaSpiManager *m_pInstance;
	static FpgaSpiManager * GetInstance();
	
	static VOID* TaskProcessEntry(VOID *p);
	void TaskProcess();
	
	static VOID* TaskSendEntry(VOID *p);
	void TaskSend();

	static VOID* TaskKvmGetEntry(VOID *p);
	void TaskKvmGet();

	static VOID* TaskPviGetEntry(VOID *p);
	void TaskPviGet();

	static VOID* TaskPviAckEntry(VOID *p);
	void TaskPviAck();

	static VOID* TaskCheckStatusEntry(VOID *p);
	void TaskCheckStatus();
	
	static SDWORD ResponeSendCbEntry(void* pUserData,const BYTE* byBuf,WORD wBufLen,void* pThis);
	SDWORD SendCmd(WORD wCmdId, BYTE byDst, BYTE *pbyBuf, WORD wLength, BYTE byFir  = FIRB_VALID_ALL);
	SDWORD SendCmdData(BYTE *pbyBuf, WORD wLength, BYTE byFir  = FIRB_VALID_ALL);
    SDWORD SendPviData(BYTE *pbyBuf, WORD wLength);
    SDWORD SendKmrData(BYTE *pbyBuf, WORD wLength);
    SDWORD SendHidData(BYTE *pbyBuf, WORD wLength);

	SDWORD SendJsonCmd(const Json::Value& jsonMsg, BYTE byFir  = FIRB_VALID_ALL);
	SDWORD SendJsonCmdToUi(const Json::Value& jsonMsg);
    SDWORD SendTestData(const string & data);

	SDWORD Init();
	SDWORD ResolutionInit();
	SDWORD UsbInit(bool bSwitch);
    void InitThrift()   override;


    static VOID* TaskReadIntStatEntry(VOID *p);
    static VOID* TaskReadRegStatEntry(VOID *p);
	static VOID* TaskReadSpiDataEntry(VOID *p);
	static VOID* TaskLinkStatEntry(VOID *p);
	

	void TaskRecv(FPGA_LOCK_TYPE_DSC_S *stTypeDsc);
    void TaskReadIntStat();
    void TaskReadRegStat();
	void TaskLinkStat();

    void StartPreviewAckThread() { m_byPviAckStatus = 1;}
    void StopPreviewAckThread() { m_byPviAckStatus = 0;}

    void OnAliveUp();

	DevID_t GetDevID();

    DevID_t GetCurDevID(){return m_wCurDevId;}
    VOID  SetCurDevID(DevID_t wDevID){ m_wCurDevId = wDevID;}

	VOID GetDevLocation1( BYTE *pbyslot, BYTE *pbyChannel){
		*pbyslot=m_bySlot1; *pbyChannel=m_byChannel1;
	}
	VOID GetDevLocation2(BYTE *pbyslot, BYTE *pbyChannel){
		*pbyslot=m_bySlot2; *pbyChannel=m_byChannel2;
	}

	VOID SetDevLocation1( BYTE byslot, BYTE byChannel);
	VOID SetDevLocation2( BYTE byslot, BYTE byChannel);

	VOID SetControlId(DevID_t wControlId) {
		m_wMControlId = wControlId;
	}
	VOID GetControlId(DevID_t *pwControlId){
		*pwControlId = m_wMControlId;
	}

	VOID SetUserId(WORD wUserId) {
		m_wLoginUserId = wUserId;
	}
	VOID GetUserId(WORD *pwUserId){
		*pwUserId = m_wLoginUserId;
	}

	VOID SetKvmId(WORD wKvmId) {
        m_wKvmId = wKvmId;
		RxInfo_st.wRelationSeat = wKvmId;
		IniWriteInt(CONNECT_INI, "KmrConnect", "KmrId", wKvmId);
	}
	VOID GetKvmId(WORD *pwKvmId){
        *pwKvmId = m_wKvmId;
	}

	VOID SetDevName(Json::String strName) {
		sDevName = strName;
	    IniWriteString(DEVINFO_INI, "DevInfo", "Device_Name", sDevName.c_str());
	    system("sync");
#if 0
        if(m_HardWareClient) {
            bool b;
            std::string key = "NAME";
            ThriftCommon::ThriftCall<HardWareServerClient, const std::string &, const std::string & >
                    (b, m_HardWareClient, &HardWareServerClient::LedSetString, key, strName);
        }
#endif
	}
	Json::String GetDevName(){
        char byTemp[128] = {0};
	    IniReadString(DEVINFO_INI, "DevInfo", "Device_Name", byTemp, sizeof(byTemp));
        sDevName = std::string((const char *)byTemp);
		return sDevName;
	}

	VOID SetVideoStat(BOOL bVideoStat) {
		VideoStat = bVideoStat;
	}
	BOOL GetVideoStat(){
		return VideoStat;
	}

    VOID SetNetStat(BYTE byNetStat) {
        m_byNetStat = byNetStat;
        if(m_QtClient) {
            bool bError = false;
            ThriftCommon::ThriftCall<QtServerClient, int32_t>
                    (bError, m_QtClient, &QtServerClient::SetNetStat, std::move(m_byNetStat));
            if(bError)
            {
                PRINT_FAULT("QtServerClient::SetNetStat (%d) failed!\n", m_byNetStat);
            }
        }
    }

    BYTE GetNetStat(){
        return m_byNetStat;
    }
	
    VOID SetKvmCtrlRxId(DevID_t wRxId) {
        m_wKvmCtrlRxId = wRxId;
    }
    VOID GetKvmCtrlRxId(DevID_t *wRxId){
        *wRxId = m_wKvmCtrlRxId;
    }
    VOID SetKvmCurRxId(DevID_t wRxId) {
        m_wKvmCurRxId = wRxId;
    }
    VOID GetKvmCurRxId(DevID_t *wRxId){
        *wRxId = m_wKvmCurRxId;
    }

	VOID SaveRxInfo(Json::Value &Json);
	VOID GetRxInfo(JsonRxInfo *RxInfo);

    DeviceStatus GetDeviceStatus();	

    void SetOsdLocatePolicy(const OSD_LOCATE_POLICY_E::type policy);
    void SetLauncherGeometry(int x, int y, int width, int height);
    void NotifyOutResolutionChanged();
    void NotifyNetStatChanged();

private:
    SDWORD SendFpgaData(SPI_INT_FLAG_E eFlag, BYTE *byBuf, WORD wLength);

	SDWORD HandleSubPack(BYTE *pbyBuf, WORD wLength);

private:
	FpgaSpiCmdAdapter *m_pFpgaSpiCmdAdpter;
    //FpgaSpiAdapter *m_pFpgaSpiAdpter;
	
	OS_TASK_DSC_S m_stTaskProcessDsc;
	OS_TASK_DSC_S m_stTaskSendDsc;
    OS_TASK_DSC_S m_stTaskInt;

	OS_TASK_DSC_S m_stTaskPviAckDsc;
    OS_TASK_DSC_S m_stTaskCheckStatusDsc;

	OS_TASK_DSC_S m_stTaskKvmGet;
	MouseMessageQueue *m_pMouseMessageQueue;
    MouseMessageQueue *m_pKmrHidRecvQueue;

    Semaphore m_SemFpgaInt;

	OS_TASK_DSC_S m_stTaskPviGet;

	QtServerClient *m_QtClient;
	UpdateServerClient *m_UpdateClient;
	HardWareServerClient *m_HardWareClient;
	Gsv2002ServerClient *m_Gsv2002Client;
    KvmServerClient *m_KvmClient;

	MSG_Q_ID m_MsgQ;
    MSG_Q_ID m_PviMsgQId;

    SDWORD m_FpgaDevFd = -1;
    DevID_t m_wCurDevId;	// 盒子ID,实时保存到文件
    DWORD m_wLoginUserId;	// 当前登录用户ID
    WORD m_wKvmId;			// 坐席ID
    DevID_t m_wKvmCtrlRxId;    //座席控制RX ID
    DevID_t m_wKvmCurRxId;     //座席当前RX ID
	DevID_t m_wMControlId;	// 主控ID
	BYTE m_bySlot1 = 0;		// 槽位号
	BYTE m_byChannel1 = 0;	// 通道号
	BYTE m_bySlot2 = 0;		// 槽位号
	BYTE m_byChannel2 = 0;	// 通道号

    OsdLocator  *m_OsdLocator;
	Json::String sDevName;
	BOOL VideoStat;
	JsonRxInfo RxInfo_st;

	SDWORD SendJsonVideoStat(bool videoStat);

    SDWORD PushMsgToMsQ(BYTE *pbyData, const DWORD &dwDataLen);
	SDWORD PushResponeToMsQ(const ITCMsgHeader* pstMsqHeader,const BYTE* &byBuf,const WORD &wBufLen);

    friend class FpgaSpiCmdAdapter;
    
    Json::FastWriter m_jsonFastWriter;
    Json::Reader m_jsonReader;
   
    //preview 
    BYTE m_byPviAckStatus = 0;
    struct timeval m_stGetPviDataBegin{};
    struct timeval m_stGetPviDataEnd{};

    WORD m_wTemp;
    WORD m_wVideoStat;
    WORD m_wHdmiStat;

    BYTE m_byNetStat = 0;
    bool m_notifyQtUi = false;
    OSD_LOCATE_POLICY_E::type m_osdLocatePolicy = OSD_LOCATE_POLICY_E::LOCATE_CENTER; 
};

#endif
