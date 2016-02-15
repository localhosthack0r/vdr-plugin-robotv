/*
 *      vdr-plugin-robotv - RoboTV server plugin for VDR
 *
 *      Copyright (C) 2015 Alexander Pipelka
 *
 *      https://github.com/pipelka/vdr-plugin-robotv
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/types.h>
#include <map>
#include <string>

#include <vdr/recording.h>
#include <vdr/channels.h>
#include <vdr/i18n.h>
#include <vdr/videodir.h>
#include <vdr/plugin.h>
#include <vdr/timers.h>
#include <vdr/menu.h>
#include <vdr/device.h>
#include <vdr/sources.h>

#include "config/config.h"
#include "live/livestreamer.h"
#include "net/msgpacket.h"
#include "recordings/recordingscache.h"
#include "recordings/packetplayer.h"
#include "tools/hash.h"
#include "tools/urlencode.h"

#include "robotv/robotvchannels.h"

#include "robotvcommand.h"
#include "robotvclient.h"
#include "robotvserver.h"
#include "timerconflicts.h"


static bool IsRadio(const cChannel* channel) {
    bool isRadio = false;

    // assume channels without VPID & APID are video channels
    if(channel->Vpid() == 0 && channel->Apid(0) == 0) {
        isRadio = false;
    }
    // channels without VPID are radio channels (channels with VPID 1 are encrypted radio channels)
    else if(channel->Vpid() == 0 || channel->Vpid() == 1) {
        isRadio = true;
    }

    return isRadio;
}

static uint32_t recid2uid(const char* recid) {
    uint32_t uid = 0;
    sscanf(recid, "%8x", &uid);
    DEBUGLOG("lookup recid: %s (uid: %u)", recid, uid);
    return uid;
}

void RoboTVClient::addChannelToPacket(const cChannel* channel, MsgPacket* p) {
    p->put_U32(channel->Number());
    p->put_String(m_toUTF8.Convert(channel->Name()));
    p->put_U32(CreateChannelUID(channel));
    p->put_U32(channel->Ca());

    // logo url
    p->put_String((const char*)CreateLogoURL(channel));

    // service reference
    if(m_protocolVersion > 4) {
        p->put_String((const char*)CreateServiceReference(channel));
    }
}

cString RoboTVClient::CreateServiceReference(const cChannel* channel) {
    int hash = 0;

    if(cSource::IsSat(channel->Source())) {
        hash = channel->Source() & cSource::st_Pos;

#if VDRVERSNUM >= 20101
        hash = -hash;
#endif

        if(hash > 0x00007FFF) {
            hash |= 0xFFFF0000;
        }

        if(hash < 0) {
            hash = -hash;
        }
        else {
            hash = 1800 + hash;
        }

        hash = hash << 16;
    }
    else if(cSource::IsCable(channel->Source())) {
        hash = 0xFFFF0000;
    }
    else if(cSource::IsTerr(channel->Source())) {
        hash = 0xEEEE0000;
    }
    else if(cSource::IsAtsc(channel->Source())) {
        hash = 0xDDDD0000;
    }

    cString serviceref = cString::sprintf("1_0_%i_%X_%X_%X_%X_0_0_0",
                                          IsRadio(channel) ? 2 : (channel->Vtype() == 27) ? 19 : 1,
                                          channel->Sid(),
                                          channel->Tid(),
                                          channel->Nid(),
                                          hash);

    return serviceref;
}

cString RoboTVClient::CreateLogoURL(const cChannel* channel) {
    std::string url = RoboTVServerConfig::instance().PiconsURL;

    if(url.empty()) {
        return "";
    }

    std::string filename = (const char*)CreateServiceReference(channel);

    if(url.size() > 4 && url.substr(0, 4) == "http") {
        filename = url_encode(filename);
    }

    cString piconurl = AddDirectory(url.c_str(), filename.c_str());
    return cString::sprintf("%s.png", (const char*)piconurl);
}

void RoboTVClient::PutTimer(cTimer* timer, MsgPacket* p) {
    int flags = CheckTimerConflicts(timer);

    p->put_U32(CreateTimerUID(timer));
    p->put_U32(timer->Flags() | flags);
    p->put_U32(timer->Priority());
    p->put_U32(timer->Lifetime());
    p->put_U32(CreateChannelUID(timer->Channel()));
    p->put_U32(timer->StartTime());
    p->put_U32(timer->StopTime());
    p->put_U32(timer->Day());
    p->put_U32(timer->WeekDays());
    p->put_String(m_toUTF8.Convert(timer->File()));
}

RoboTVClient::RoboTVClient(int fd, unsigned int id) {
    m_Id                      = id;
    m_loggedIn                = false;
    m_Streamer                = NULL;
    m_StatusInterfaceEnabled  = false;
    m_RecPlayer               = NULL;
    m_req                     = NULL;
    m_resp                    = NULL;
    m_compressionLevel        = 0;
    m_LanguageIndex           = -1;
    m_LangStreamType          = StreamInfo::stMPEG2AUDIO;
    m_channelCount            = 0;
    m_timeout                 = 3000;
    m_scanSupported           = false;

    m_socket = fd;
    m_wantfta = true;
    m_filterlanguage = false;

    Start();

    m_scanSupported = m_scanner.Connect();
}

RoboTVClient::~RoboTVClient() {
    DEBUGLOG("%s", __FUNCTION__);
    StopChannelStreaming();

    // shutdown connection
    shutdown(m_socket, SHUT_RDWR);
    Cancel(10);

    // close connection
    close(m_socket);

    // remove recplayer
    delete m_RecPlayer;

    // delete messagequeue
    {
        cMutexLock lock(&m_queueLock);

        while(!m_queue.empty()) {
            MsgPacket* p = m_queue.front();
            m_queue.pop();
            delete p;
        }
    }

    DEBUGLOG("done");
}

void RoboTVClient::Action(void) {
    bool bClosed(false);

    // only root may change the priority
    if(geteuid() == 0) {
        SetPriority(10);
    }

    while(Running()) {

        // send pending messages
        {
            cMutexLock lock(&m_queueLock);

            while(!m_queue.empty()) {
                MsgPacket* p = m_queue.front();

                if(!p->write(m_socket, m_timeout)) {
                    break;
                }

                m_queue.pop();
                delete p;
            }
        }

        m_req = MsgPacket::read(m_socket, bClosed, 1000);

        if(bClosed) {
            delete m_req;
            m_req = NULL;
            break;
        }

        if(m_req != NULL) {
            processRequest();
            delete m_req;
        }
        else if(m_scanner.IsScanning()) {
            SendScannerStatus();
        }
    }

    /* If thread is ended due to closed connection delete a
       possible running stream here */
    StopChannelStreaming();
}

int RoboTVClient::StartChannelStreaming(const cChannel* channel, uint32_t timeout, int32_t priority, bool waitforiframe, bool rawPTS) {
    cMutexLock lock(&m_streamerLock);

    m_Streamer = new LiveStreamer(this, channel, priority, rawPTS);
    m_Streamer->setLanguage(m_LanguageIndex, m_LangStreamType);
    m_Streamer->setTimeout(timeout);
    m_Streamer->setProtocolVersion(m_protocolVersion);
    m_Streamer->setWaitForKeyFrame(waitforiframe);

    return ROBOTV_RET_OK;
}

void RoboTVClient::StopChannelStreaming() {
    cMutexLock lock(&m_streamerLock);

    delete m_Streamer;
    m_Streamer = NULL;
}

void RoboTVClient::TimerChange(const cTimer* Timer, eTimerChange Change) {
    // ignore invalid timers
    if(Timer == NULL) {
        return;
    }

    TimerChange();
}

void RoboTVClient::ChannelChange(const cChannel* Channel) {
    cMutexLock lock(&m_streamerLock);

    INFOLOG("ChannelChange: %i - %s", Channel->Number(), Channel->ShortName());

    if(m_Streamer != NULL) {
        m_Streamer->channelChange(Channel);
    }

    cMutexLock msgLock(&m_msgLock);

    if(m_StatusInterfaceEnabled && m_protocolVersion >= 6) {
        MsgPacket* resp = new MsgPacket(ROBOTV_STATUS_CHANNELCHANGED, ROBOTV_CHANNEL_STATUS);
        addChannelToPacket(Channel, resp);
        QueueMessage(resp);
    }
}

void RoboTVClient::TimerChange() {
    cMutexLock lock(&m_msgLock);

    if(m_StatusInterfaceEnabled) {
        INFOLOG("Sending timer change request to client #%i ...", m_Id);
        MsgPacket* resp = new MsgPacket(ROBOTV_STATUS_TIMERCHANGE, ROBOTV_CHANNEL_STATUS);
        QueueMessage(resp);
    }
}

void RoboTVClient::ChannelsChanged() {
    cMutexLock lock(&m_msgLock);

    if(!m_StatusInterfaceEnabled) {
        return;
    }

    int count = ChannelsCount();

    if(m_channelCount == count) {
        INFOLOG("Client %i: %i channels, no change", m_Id, count);
        return;
    }

    if(m_channelCount == 0) {
        INFOLOG("Client %i: no channels - sending request", m_Id);
    }
    else {
        INFOLOG("Client %i : %i channels, %i available - sending request", m_Id, m_channelCount, count);
    }

    MsgPacket* resp = new MsgPacket(ROBOTV_STATUS_CHANNELCHANGE, ROBOTV_CHANNEL_STATUS);
    QueueMessage(resp);
}

void RoboTVClient::RecordingsChange() {
    cMutexLock lock(&m_msgLock);

    if(!m_StatusInterfaceEnabled) {
        return;
    }

    MsgPacket* resp = new MsgPacket(ROBOTV_STATUS_RECORDINGSCHANGE, ROBOTV_CHANNEL_STATUS);
    QueueMessage(resp);
}

void RoboTVClient::Recording(const cDevice* Device, const char* Name, const char* FileName, bool On) {
    cMutexLock lock(&m_msgLock);

    if(m_StatusInterfaceEnabled) {
        MsgPacket* resp = new MsgPacket(ROBOTV_STATUS_RECORDING, ROBOTV_CHANNEL_STATUS);

        resp->put_U32(Device->CardIndex());
        resp->put_U32(On);

        if(Name) {
            resp->put_String(Name);
        }
        else {
            resp->put_String("");
        }

        if(FileName) {
            resp->put_String(FileName);
        }
        else {
            resp->put_String("");
        }

        QueueMessage(resp);
    }
}

void RoboTVClient::OsdStatusMessage(const char* Message) {
    cMutexLock lock(&m_msgLock);

    if(m_StatusInterfaceEnabled && Message) {
        /* Ignore this messages */
        if(strcasecmp(Message, trVDR("Channel not available!")) == 0) {
            return;
        }
        else if(strcasecmp(Message, trVDR("Delete timer?")) == 0) {
            return;
        }
        else if(strcasecmp(Message, trVDR("Delete recording?")) == 0) {
            return;
        }
        else if(strcasecmp(Message, trVDR("Press any key to cancel shutdown")) == 0) {
            return;
        }
        else if(strcasecmp(Message, trVDR("Press any key to cancel restart")) == 0) {
            return;
        }
        else if(strcasecmp(Message, trVDR("Editing - shut down anyway?")) == 0) {
            return;
        }
        else if(strcasecmp(Message, trVDR("Recording - shut down anyway?")) == 0) {
            return;
        }
        else if(strcasecmp(Message, trVDR("shut down anyway?")) == 0) {
            return;
        }
        else if(strcasecmp(Message, trVDR("Recording - restart anyway?")) == 0) {
            return;
        }
        else if(strcasecmp(Message, trVDR("Editing - restart anyway?")) == 0) {
            return;
        }
        else if(strcasecmp(Message, trVDR("Delete channel?")) == 0) {
            return;
        }
        else if(strcasecmp(Message, trVDR("Timer still recording - really delete?")) == 0) {
            return;
        }
        else if(strcasecmp(Message, trVDR("Delete marks information?")) == 0) {
            return;
        }
        else if(strcasecmp(Message, trVDR("Delete resume information?")) == 0) {
            return;
        }
        else if(strcasecmp(Message, trVDR("CAM is in use - really reset?")) == 0) {
            return;
        }
        else if(strcasecmp(Message, trVDR("Really restart?")) == 0) {
            return;
        }
        else if(strcasecmp(Message, trVDR("Stop recording?")) == 0) {
            return;
        }
        else if(strcasecmp(Message, trVDR("Cancel editing?")) == 0) {
            return;
        }
        else if(strcasecmp(Message, trVDR("Cutter already running - Add to cutting queue?")) == 0) {
            return;
        }
        else if(strcasecmp(Message, trVDR("No index-file found. Creating may take minutes. Create one?")) == 0) {
            return;
        }

        StatusMessage(Message);
    }
}

void RoboTVClient::StatusMessage(const char* Message) {
    MsgPacket* resp = new MsgPacket(ROBOTV_STATUS_MESSAGE, ROBOTV_CHANNEL_STATUS);

    resp->put_U32(0);
    resp->put_String(Message);

    QueueMessage(resp);
}

bool RoboTVClient::IsChannelWanted(cChannel* channel, int type) {
    // dismiss invalid channels
    if(channel == NULL) {
        return false;
    }

    // radio
    if((type == 1) && !IsRadio(channel)) {
        return false;
    }

    // (U)HD channels
    if((type == 2) && (channel->Vtype() != 27 && channel->Vtype() != 36)) {
        return false;
    }

    // skip channels witout SID
    if(channel->Sid() == 0) {
        return false;
    }

    if(strcmp(channel->Name(), ".") == 0) {
        return false;
    }

    // check language
    if(m_filterlanguage && m_LanguageIndex != -1) {
        bool bLanguageFound = false;
        const char* lang = NULL;

        // check MP2 languages
        for(int i = 0; i < MAXAPIDS; i++) {
            lang = channel->Alang(i);

            if(lang == NULL) {
                break;
            }

            if(m_LanguageIndex == I18nLanguageIndex(lang)) {
                bLanguageFound = true;
                break;
            }
        }

        // check other digital languages
        for(int i = 0; i < MAXDPIDS; i++) {
            lang = channel->Dlang(i);

            if(lang == NULL) {
                break;
            }

            if(m_LanguageIndex == I18nLanguageIndex(lang)) {
                bLanguageFound = true;
                break;
            }
        }

        if(!bLanguageFound) {
            return false;
        }
    }

    // user selection for FTA channels
    if(channel->Ca(0) == 0) {
        return m_wantfta;
    }

    // we want all encrypted channels if there isn't any CaID filter
    if(m_caids.size() == 0) {
        return true;
    }

    // check if we have a matching CaID
    for(std::list<int>::iterator i = m_caids.begin(); i != m_caids.end(); i++) {
        for(int j = 0; j < MAXCAIDS; j++) {

            if(channel->Ca(j) == 0) {
                break;
            }

            if(channel->Ca(j) == *i) {
                return true;
            }
        }
    }

    return false;
}

bool RoboTVClient::processRequest() {
    cMutexLock lock(&m_msgLock);

    m_resp = new MsgPacket(m_req->getMsgID(), ROBOTV_CHANNEL_REQUEST_RESPONSE, m_req->getUID());
    m_resp->setProtocolVersion(ROBOTV_PROTOCOLVERSION);

    bool result = false;

    switch(m_req->getMsgID()) {
            /** OPCODE 1 - 19: RoboTV network functions for general purpose */
        case ROBOTV_LOGIN:
            result = process_Login();
            break;

        case ROBOTV_GETTIME:
            result = process_GetTime();
            break;

        case ROBOTV_ENABLESTATUSINTERFACE:
            result = process_EnableStatusInterface();
            break;

        case ROBOTV_UPDATECHANNELS:
            result = process_UpdateChannels();
            break;

        case ROBOTV_CHANNELFILTER:
            result = process_ChannelFilter();
            break;

            /** OPCODE 20 - 39: RoboTV network functions for live streaming */
        case ROBOTV_CHANNELSTREAM_OPEN:
            result = processChannelStream_Open();
            break;

        case ROBOTV_CHANNELSTREAM_CLOSE:
            result = processChannelStream_Close();
            break;

        case ROBOTV_CHANNELSTREAM_REQUEST:
            result = processChannelStream_Request();
            break;

        case ROBOTV_CHANNELSTREAM_PAUSE:
            result = processChannelStream_Pause();
            break;

        case ROBOTV_CHANNELSTREAM_SIGNAL:
            result = processChannelStream_Signal();
            break;

            /** OPCODE 40 - 59: RoboTV network functions for recording streaming */
        case ROBOTV_RECSTREAM_OPEN:
            result = processRecStream_Open();
            break;

        case ROBOTV_RECSTREAM_CLOSE:
            result = processRecStream_Close();
            break;

        case ROBOTV_RECSTREAM_GETBLOCK:
            result = processRecStream_GetBlock();
            break;

        case ROBOTV_RECSTREAM_GETPACKET:
            result = processRecStream_GetPacket();
            break;

        case ROBOTV_RECSTREAM_UPDATE:
            result = processRecStream_Update();
            break;

        case ROBOTV_RECSTREAM_SEEK:
            result = processRecStream_Seek();
            break;


            /** OPCODE 60 - 79: RoboTV network functions for channel access */
        case ROBOTV_CHANNELS_GETCOUNT:
            result = processCHANNELS_ChannelsCount();
            break;

        case ROBOTV_CHANNELS_GETCHANNELS:
            result = processCHANNELS_GetChannels();
            break;

        case ROBOTV_CHANNELGROUP_GETCOUNT:
            result = processCHANNELS_GroupsCount();
            break;

        case ROBOTV_CHANNELGROUP_LIST:
            result = processCHANNELS_GroupList();
            break;

        case ROBOTV_CHANNELGROUP_MEMBERS:
            result = processCHANNELS_GetGroupMembers();
            break;

            /** OPCODE 80 - 99: RoboTV network functions for timer access */
        case ROBOTV_TIMER_GETCOUNT:
            result = processTIMER_GetCount();
            break;

        case ROBOTV_TIMER_GET:
            result = processTIMER_Get();
            break;

        case ROBOTV_TIMER_GETLIST:
            result = processTIMER_GetList();
            break;

        case ROBOTV_TIMER_ADD:
            result = processTIMER_Add();
            break;

        case ROBOTV_TIMER_DELETE:
            result = processTIMER_Delete();
            break;

        case ROBOTV_TIMER_UPDATE:
            result = processTIMER_Update();
            break;


            /** OPCODE 100 - 119: RoboTV network functions for recording access */
        case ROBOTV_RECORDINGS_DISKSIZE:
            result = processRECORDINGS_GetDiskSpace();
            break;

        case ROBOTV_RECORDINGS_GETCOUNT:
            result = processRECORDINGS_GetCount();
            break;

        case ROBOTV_RECORDINGS_GETLIST:
            result = processRECORDINGS_GetList();
            break;

        case ROBOTV_RECORDINGS_RENAME:
            result = processRECORDINGS_Rename();
            break;

        case ROBOTV_RECORDINGS_DELETE:
            result = processRECORDINGS_Delete();
            break;

        case ROBOTV_RECORDINGS_SETPLAYCOUNT:
            result = processRECORDINGS_SetPlayCount();
            break;

        case ROBOTV_RECORDINGS_SETPOSITION:
            result = processRECORDINGS_SetPosition();
            break;

        case ROBOTV_RECORDINGS_SETURLS:
            result = processRECORDINGS_SetUrls();
            break;

        case ROBOTV_RECORDINGS_GETPOSITION:
            result = processRECORDINGS_GetPosition();
            break;

        case ROBOTV_RECORDINGS_GETMARKS:
            result = processRECORDINGS_GetMarks();
            break;

        case ROBOTV_ARTWORK_GET:
            result = processArtwork_Get();
            break;

        case ROBOTV_ARTWORK_SET:
            result = processArtwork_Set();
            break;

            /** OPCODE 120 - 139: RoboTV network functions for epg access and manipulating */
        case ROBOTV_EPG_GETFORCHANNEL:
            result = processEPG_GetForChannel();
            break;


            /** OPCODE 140 - 159: RoboTV network functions for channel scanning */
        case ROBOTV_SCAN_SUPPORTED:
            result = processSCAN_ScanSupported();
            break;

        case ROBOTV_SCAN_GETSETUP:
            result = processSCAN_GetSetup();
            break;

        case ROBOTV_SCAN_SETSETUP:
            result = processSCAN_SetSetup();
            break;

        case ROBOTV_SCAN_START:
            result = processSCAN_Start();
            break;

        case ROBOTV_SCAN_STOP:
            result = processSCAN_Stop();
            break;

        case ROBOTV_SCAN_GETSTATUS:
            result = processSCAN_GetStatus();
            break;

        default:
            break;
    }

    if(result) {
        QueueMessage(m_resp);
    }

    m_resp = NULL;

    return result;
}


/** OPCODE 1 - 19: RoboTV network functions for general purpose */

bool RoboTVClient::process_Login() { /* OPCODE 1 */
    m_protocolVersion = m_req->getProtocolVersion();
    m_compressionLevel = m_req->get_U8();
    m_clientName = m_req->get_String();
    const char* language   = NULL;

    // get preferred language
    if(!m_req->eop()) {
        language = m_req->get_String();
        m_LanguageIndex = I18nLanguageIndex(language);
        m_LangStreamType = (StreamInfo::Type)m_req->get_U8();
    }

    if(m_protocolVersion > ROBOTV_PROTOCOLVERSION || m_protocolVersion < 4) {
        ERRORLOG("Client '%s' has unsupported protocol version '%u', terminating client", m_clientName.c_str(), m_protocolVersion);
        return false;
    }

    INFOLOG("Welcome client '%s' with protocol version '%u'", m_clientName.c_str(), m_protocolVersion);

    if(!m_LanguageIndex != -1) {
        INFOLOG("Preferred language: %s / type: %i", I18nLanguageCode(m_LanguageIndex), (int)m_LangStreamType);
    }

    // Send the login reply
    time_t timeNow        = time(NULL);
    struct tm* timeStruct = localtime(&timeNow);
    int timeOffset        = timeStruct->tm_gmtoff;

    m_resp->setProtocolVersion(m_protocolVersion);
    m_resp->put_U32(timeNow);
    m_resp->put_S32(timeOffset);
    m_resp->put_String("VDR-RoboTV Server");
    m_resp->put_String(ROBOTV_VERSION);

    SetLoggedIn(true);
    return true;
}

bool RoboTVClient::process_GetTime() { /* OPCODE 2 */
    time_t timeNow        = time(NULL);
    struct tm* timeStruct = localtime(&timeNow);
    int timeOffset        = timeStruct->tm_gmtoff;

    m_resp->put_U32(timeNow);
    m_resp->put_S32(timeOffset);

    return true;
}

bool RoboTVClient::process_EnableStatusInterface() {
    bool enabled = m_req->get_U8();

    SetStatusInterface(enabled);

    m_resp->put_U32(ROBOTV_RET_OK);

    return true;
}

bool RoboTVClient::process_UpdateChannels() {
    uint8_t updatechannels = m_req->get_U8();

    if(updatechannels <= 5) {
        Setup.UpdateChannels = updatechannels;
        INFOLOG("Setting channel update method: %i", updatechannels);
        m_resp->put_U32(ROBOTV_RET_OK);
    }
    else {
        m_resp->put_U32(ROBOTV_RET_DATAINVALID);
    }

    return true;
}

bool RoboTVClient::process_ChannelFilter() {
    INFOLOG("Channellist filter:");

    // do we want fta channels ?
    m_wantfta = m_req->get_U32();
    INFOLOG("Free To Air channels: %s", m_wantfta ? "Yes" : "No");

    // display only channels with native language audio ?
    m_filterlanguage = m_req->get_U32();
    INFOLOG("Only native language: %s", m_filterlanguage ? "Yes" : "No");

    // read caids
    m_caids.clear();
    uint32_t count = m_req->get_U32();

    INFOLOG("Enabled CaIDs: ");

    // sanity check (maximum of 20 caids)
    if(count < 20) {
        for(uint32_t i = 0; i < count; i++) {
            int caid = m_req->get_U32();
            m_caids.push_back(caid);
            INFOLOG("%04X", caid);
        }
    }


    m_resp->put_U32(ROBOTV_RET_OK);

    return true;
}



/** OPCODE 20 - 39: RoboTV network functions for live streaming */

bool RoboTVClient::processChannelStream_Open() { /* OPCODE 20 */
    // only root may change the priority
    if(geteuid() == 0) {
        SetPriority(-15);
    }

    uint32_t uid = m_req->get_U32();
    int32_t priority = 50;
    bool waitforiframe = false;
    bool rawPTS = false;

    if(!m_req->eop()) {
        priority = m_req->get_S32();
    }

    if(!m_req->eop()) {
        waitforiframe = m_req->get_U8();
    }

    if(!m_req->eop()) {
        rawPTS = m_req->get_U8();
    }

    uint32_t timeout = RoboTVServerConfig::instance().stream_timeout;

    StopChannelStreaming();

    RoboTVChannels& c = RoboTVChannels::instance();
    c.Lock(false);
    const cChannel* channel = NULL;

    // try to find channel by uid first
    channel = FindChannelByUID(uid);

    // try channelnumber
    if(channel == NULL) {
        channel = c.Get()->GetByNumber(uid);
    }

    c.Unlock();

    if(channel == NULL) {
        ERRORLOG("Can't find channel %08x", uid);
        m_resp->put_U32(ROBOTV_RET_DATAINVALID);
    }
    else {
        int status = StartChannelStreaming(channel, timeout, priority, waitforiframe, rawPTS);

        if(status == ROBOTV_RET_OK) {
            INFOLOG("--------------------------------------");
            INFOLOG("Started streaming of channel %s (timeout %i seconds, priority %i)", channel->Name(), timeout, priority);
        }
        else {
            DEBUGLOG("Can't stream channel %s", channel->Name());
        }

        m_resp->put_U32(status);
    }

    return true;
}

bool RoboTVClient::processChannelStream_Close() { /* OPCODE 21 */
    StopChannelStreaming();
    return true;
}

bool RoboTVClient::processChannelStream_Request() { /* OPCODE 22 */
    if(m_Streamer != NULL) {
        m_Streamer->requestPacket();
    }

    // no response needed for the request
    return false;
}

bool RoboTVClient::processChannelStream_Pause() { /* OPCODE 23 */
    bool on = m_req->get_U32();
    INFOLOG("LIVESTREAM: %s", on ? "PAUSED" : "TIMESHIFT");

    m_Streamer->pause(on);

    return true;
}

bool RoboTVClient::processChannelStream_Signal() { /* OPCODE 24 */
    if(m_Streamer == NULL) {
        return false;
    }

    m_Streamer->requestSignalInfo();
    return false;
}

/** OPCODE 40 - 59: RoboTV network functions for recording streaming */

bool RoboTVClient::processRecStream_Open() { /* OPCODE 40 */
    cRecording* recording = NULL;

    // only root may change the priority
    if(geteuid() == 0) {
        SetPriority(-15);
    }

    const char* recid = m_req->get_String();
    unsigned int uid = recid2uid(recid);
    DEBUGLOG("lookup recid: %s (uid: %u)", recid, uid);
    recording = RecordingsCache::GetInstance().Lookup(uid);

    if(recording && m_RecPlayer == NULL) {
        m_RecPlayer = new PacketPlayer(recording);

        m_resp->put_U32(ROBOTV_RET_OK);
        m_resp->put_U32(0);
        m_resp->put_U64(m_RecPlayer->getLengthBytes());
#if VDRVERSNUM < 10703
        m_resp->put_U8(true);//added for TS
#else
        m_resp->put_U8(recording->IsPesRecording());//added for TS
#endif
        m_resp->put_U32(recording->LengthInSeconds());
    }
    else {
        m_resp->put_U32(ROBOTV_RET_DATAUNKNOWN);
        ERRORLOG("%s - unable to start recording !", __FUNCTION__);
    }

    return true;
}

bool RoboTVClient::processRecStream_Close() { /* OPCODE 41 */
    if(m_RecPlayer) {
        delete m_RecPlayer;
        m_RecPlayer = NULL;
    }

    m_resp->put_U32(ROBOTV_RET_OK);

    return true;
}

bool RoboTVClient::processRecStream_Update() { /* OPCODE 46 */
    if(m_RecPlayer == NULL) {
        return false;
    }

    m_RecPlayer->update();
    m_resp->put_U32(0);
    m_resp->put_U64(m_RecPlayer->getLengthBytes());

    return true;
}

bool RoboTVClient::processRecStream_GetBlock() { /* OPCODE 42 */
    if(!m_RecPlayer) {
        ERRORLOG("Get block called when no recording open");
        return false;
    }

    uint64_t position  = m_req->get_U64();
    uint32_t amount    = m_req->get_U32();

    uint8_t* p = m_resp->reserve(amount);
    uint32_t amountReceived = m_RecPlayer->getBlock(p, position, amount);

    // smaller chunk ?
    if(amountReceived < amount) {
        m_resp->unreserve(amount - amountReceived);
    }

    return true;
}

bool RoboTVClient::processRecStream_GetPacket() {
    if(!m_RecPlayer) {
        return false;
    }

    MsgPacket* p = m_RecPlayer->getPacket();

    if(p == NULL) {
        return true;
    }

    QueueMessage(p);

    // taint response packet
    /*p->setType(m_req->getType());
    p->setUID(m_req->getUID());
    p->setProtocolVersion(ROBOTV_PROTOCOLVERSION);

    // inject new response
    delete m_resp;
    m_resp = p;*/

    return true;
}

bool RoboTVClient::processRecStream_Seek() {
    if(!m_RecPlayer) {
        return false;
    }

    uint64_t position = m_req->get_U64();
    int64_t pts = m_RecPlayer->seek(position);

    m_resp->put_U64(pts);

    return true;
}

int RoboTVClient::ChannelsCount() {
    RoboTVChannels& c = RoboTVChannels::instance();
    c.Lock(false);
    cChannels* channels = c.Get();
    int count = 0;

    for(cChannel* channel = channels->First(); channel; channel = channels->Next(channel)) {
        if(IsChannelWanted(channel, false)) {
            count++;
        }

        if(IsChannelWanted(channel, true)) {
            count++;
        }
    }

    c.Unlock();
    return count;
}

/** OPCODE 60 - 79: RoboTV network functions for channel access */

bool RoboTVClient::processCHANNELS_ChannelsCount() { /* OPCODE 61 */
    m_channelCount = ChannelsCount();
    m_resp->put_U32(m_channelCount);

    return true;
}

bool RoboTVClient::processCHANNELS_GetChannels() { /* OPCODE 63 */
    int type = m_req->get_U32();

    RoboTVChannels& c = RoboTVChannels::instance();
    m_channelCount = ChannelsCount();

    if(!c.Lock(false)) {
        return true;
    }

    cChannels* channels = c.Get();

    for(cChannel* channel = channels->First(); channel; channel = channels->Next(channel)) {
        if(!IsChannelWanted(channel, type)) {
            continue;
        }

        m_resp->put_U32(channel->Number());
        m_resp->put_String(m_toUTF8.Convert(channel->Name()));
        m_resp->put_U32(CreateChannelUID(channel));
        m_resp->put_U32(channel->Ca());

        // logo url
        m_resp->put_String((const char*)CreateLogoURL(channel));

        // service reference
        if(m_protocolVersion > 4) {
            m_resp->put_String((const char*)CreateServiceReference(channel));
        }
    }

    c.Unlock();

    m_resp->compress(m_compressionLevel);

    return true;
}

bool RoboTVClient::processCHANNELS_GroupsCount() {
    uint32_t type = m_req->get_U32();
    RoboTVChannels& c = RoboTVChannels::instance();

    c.Lock(false);

    m_channelgroups[0].clear();
    m_channelgroups[1].clear();

    switch(type) {
            // get groups defined in channels.conf
        default:
        case 0:
            CreateChannelGroups(false);
            break;

            // automatically create groups
        case 1:
            CreateChannelGroups(true);
            break;
    }

    c.Unlock();

    uint32_t count = m_channelgroups[0].size() + m_channelgroups[1].size();

    m_resp->put_U32(count);

    return true;
}

bool RoboTVClient::processCHANNELS_GroupList() {
    uint32_t radio = m_req->get_U8();
    std::map<std::string, ChannelGroup>::iterator i;

    for(i = m_channelgroups[radio].begin(); i != m_channelgroups[radio].end(); i++) {
        m_resp->put_String(i->second.name.c_str());
        m_resp->put_U8(i->second.radio);
    }

    return true;
}

bool RoboTVClient::processCHANNELS_GetGroupMembers() {
    const char* groupname = m_req->get_String();
    uint32_t radio = m_req->get_U8();
    int index = 0;

    // unknown group
    if(m_channelgroups[radio].find(groupname) == m_channelgroups[radio].end()) {
        return true;
    }

    bool automatic = m_channelgroups[radio][groupname].automatic;
    std::string name;

    m_channelCount = ChannelsCount();

    RoboTVChannels& c = RoboTVChannels::instance();
    c.Lock(false);
    cChannels* channels = c.Get();

    for(cChannel* channel = channels->First(); channel; channel = channels->Next(channel)) {

        if(automatic && !channel->GroupSep()) {
            name = channel->Provider();
        }
        else {
            if(channel->GroupSep()) {
                name = channel->Name();
                continue;
            }
        }

        if(name.empty()) {
            continue;
        }

        if(!IsChannelWanted(channel, radio)) {
            continue;
        }

        if(name == groupname) {
            m_resp->put_U32(CreateChannelUID(channel));
            m_resp->put_U32(++index);
        }
    }

    c.Unlock();
    return true;
}

void RoboTVClient::CreateChannelGroups(bool automatic) {
    std::string groupname;
    RoboTVChannels& c = RoboTVChannels::instance();
    cChannels* channels = c.Get();

    for(cChannel* channel = channels->First(); channel; channel = channels->Next(channel)) {
        bool isRadio = IsRadio(channel);

        if(automatic && !channel->GroupSep()) {
            groupname = channel->Provider();
        }
        else if(!automatic && channel->GroupSep()) {
            groupname = channel->Name();
        }

        if(groupname.empty()) {
            continue;
        }

        if(!IsChannelWanted(channel, isRadio)) {
            continue;
        }

        if(m_channelgroups[isRadio].find(groupname) == m_channelgroups[isRadio].end()) {
            ChannelGroup group;
            group.name = groupname;
            group.radio = isRadio;
            group.automatic = automatic;
            m_channelgroups[isRadio][groupname] = group;
        }
    }
}

/** OPCODE 80 - 99: RoboTV network functions for timer access */

bool RoboTVClient::processTIMER_GetCount() { /* OPCODE 80 */
    int count = Timers.Count();

    m_resp->put_U32(count);

    return true;
}

bool RoboTVClient::processTIMER_Get() { /* OPCODE 81 */
    uint32_t number = m_req->get_U32();

    if(Timers.Count() == 0) {
        m_resp->put_U32(ROBOTV_RET_DATAUNKNOWN);
        return true;
    }

    cTimer* timer = Timers.Get(number - 1);

    if(timer == NULL) {
        m_resp->put_U32(ROBOTV_RET_DATAUNKNOWN);
        return true;
    }

    m_resp->put_U32(ROBOTV_RET_OK);
    PutTimer(timer, m_resp);

    return true;
}

bool RoboTVClient::processTIMER_GetList() { /* OPCODE 82 */
    if(Timers.BeingEdited()) {
        ERRORLOG("Unable to delete timer - timers being edited at VDR");
        m_resp->put_U32(ROBOTV_RET_DATALOCKED);
        return true;
    }

    cTimer* timer;
    int numTimers = Timers.Count();

    m_resp->put_U32(numTimers);

    for(int i = 0; i < numTimers; i++) {
        timer = Timers.Get(i);

        if(!timer) {
            continue;
        }

        PutTimer(timer, m_resp);
    }

    return true;
}

bool RoboTVClient::processTIMER_Add() { /* OPCODE 83 */
    if(Timers.BeingEdited()) {
        ERRORLOG("Unable to add timer - timers being edited at VDR");
        m_resp->put_U32(ROBOTV_RET_DATALOCKED);
        return true;
    }

    m_req->get_U32(); // index unused
    uint32_t flags      = m_req->get_U32() > 0 ? tfActive : tfNone;
    uint32_t priority   = m_req->get_U32();
    uint32_t lifetime   = m_req->get_U32();
    uint32_t channelid  = m_req->get_U32();
    time_t startTime    = m_req->get_U32();
    time_t stopTime     = m_req->get_U32();
    time_t day          = m_req->get_U32();
    uint32_t weekdays   = m_req->get_U32();
    const char* file    = m_req->get_String();
    const char* aux     = m_req->get_String();

    // handle instant timers
    if(startTime == -1 || startTime == 0) {
        startTime = time(NULL);
    }

    struct tm tm_r;

    struct tm* time = localtime_r(&startTime, &tm_r);

    if(day <= 0) {
        day = cTimer::SetTime(startTime, 0);
    }

    int start = time->tm_hour * 100 + time->tm_min;
    time = localtime_r(&stopTime, &tm_r);
    int stop = time->tm_hour * 100 + time->tm_min;

    cString buffer;
    RoboTVChannels& c = RoboTVChannels::instance();
    c.Lock(false);

    const cChannel* channel = FindChannelByUID(channelid);

    if(channel != NULL) {
        buffer = cString::sprintf("%u:%s:%s:%04d:%04d:%d:%d:%s:%s\n", flags, (const char*)channel->GetChannelID().ToString(), *cTimer::PrintDay(day, weekdays, true), start, stop, priority, lifetime, file, aux);
    }

    c.Unlock();

    cTimer* timer = new cTimer;

    if(timer->Parse(buffer)) {
        cTimer* t = Timers.GetTimer(timer);

        if(!t) {
            Timers.Add(timer);
            Timers.SetModified();
            INFOLOG("Timer %s added", *timer->ToDescr());
            m_resp->put_U32(ROBOTV_RET_OK);
            return true;
        }
        else {
            ERRORLOG("Timer already defined: %d %s", t->Index() + 1, *t->ToText());
            m_resp->put_U32(ROBOTV_RET_DATALOCKED);
        }
    }
    else {
        ERRORLOG("Error in timer settings");
        m_resp->put_U32(ROBOTV_RET_DATAINVALID);
    }

    delete timer;

    return true;
}

bool RoboTVClient::processTIMER_Delete() { /* OPCODE 84 */
    uint32_t uid = m_req->get_U32();
    bool     force  = m_req->get_U32();

    cTimer* timer = FindTimerByUID(uid);

    if(timer == NULL) {
        ERRORLOG("Unable to delete timer - invalid timer identifier");
        m_resp->put_U32(ROBOTV_RET_DATAINVALID);
        return true;
    }

    if(Timers.BeingEdited()) {
        ERRORLOG("Unable to delete timer - timers being edited at VDR");
        m_resp->put_U32(ROBOTV_RET_DATALOCKED);
        return true;
    }

    if(timer->Recording() && !force) {
        ERRORLOG("Timer is recording and can be deleted (use force to stop it)");
        m_resp->put_U32(ROBOTV_RET_RECRUNNING);
        return true;
    }

    timer->Skip();
    cRecordControls::Process(time(NULL));

    INFOLOG("Deleting timer %s", *timer->ToDescr());
    Timers.Del(timer);
    Timers.SetModified();
    m_resp->put_U32(ROBOTV_RET_OK);

    return true;
}

bool RoboTVClient::processTIMER_Update() { /* OPCODE 85 */
    uint32_t uid = m_req->get_U32();
    bool active = m_req->get_U32();

    cTimer* timer = FindTimerByUID(uid);

    if(timer == NULL) {
        ERRORLOG("Timer not defined");
        m_resp->put_U32(ROBOTV_RET_DATAUNKNOWN);
        return true;
    }

    if(timer->Recording()) {
        INFOLOG("Will not update timer - currently recording");
        m_resp->put_U32(ROBOTV_RET_OK);
        return true;
    }

    cTimer t = *timer;

    uint32_t flags      = active ? tfActive : tfNone;
    uint32_t priority   = m_req->get_U32();
    uint32_t lifetime   = m_req->get_U32();
    uint32_t channelid  = m_req->get_U32();
    time_t startTime    = m_req->get_U32();
    time_t stopTime     = m_req->get_U32();
    time_t day          = m_req->get_U32();
    uint32_t weekdays   = m_req->get_U32();
    const char* file    = m_req->get_String();
    const char* aux     = m_req->get_String();

    struct tm tm_r;
    struct tm* time = localtime_r(&startTime, &tm_r);

    if(day <= 0) {
        day = cTimer::SetTime(startTime, 0);
    }

    int start = time->tm_hour * 100 + time->tm_min;
    time = localtime_r(&stopTime, &tm_r);
    int stop = time->tm_hour * 100 + time->tm_min;

    cString buffer;
    RoboTVChannels& c = RoboTVChannels::instance();
    c.Lock(false);

    const cChannel* channel = FindChannelByUID(channelid);

    if(channel != NULL) {
        buffer = cString::sprintf("%u:%s:%s:%04d:%04d:%d:%d:%s:%s\n", flags, (const char*)channel->GetChannelID().ToString(), *cTimer::PrintDay(day, weekdays, true), start, stop, priority, lifetime, file, aux);
    }

    c.Unlock();

    if(!t.Parse(buffer)) {
        ERRORLOG("Error in timer settings");
        m_resp->put_U32(ROBOTV_RET_DATAINVALID);
        return true;
    }

    *timer = t;
    Timers.SetModified();
    TimerChange();

    m_resp->put_U32(ROBOTV_RET_OK);

    return true;
}


/** OPCODE 100 - 119: RoboTV network functions for recording access */

bool RoboTVClient::processRECORDINGS_GetDiskSpace() { /* OPCODE 100 */
    int FreeMB;
#if VDRVERSNUM >= 20102
    int Percent = cVideoDirectory::VideoDiskSpace(&FreeMB);
#else
    int Percent = VideoDiskSpace(&FreeMB);
#endif
    int Total   = (FreeMB / (100 - Percent)) * 100;

    m_resp->put_U32(Total);
    m_resp->put_U32(FreeMB);
    m_resp->put_U32(Percent);

    return true;
}

bool RoboTVClient::processRECORDINGS_GetCount() { /* OPCODE 101 */
    m_resp->put_U32(Recordings.Count());

    return true;
}

bool RoboTVClient::processRECORDINGS_GetList() { /* OPCODE 102 */
    RecordingsCache& reccache = RecordingsCache::GetInstance();

    for(cRecording* recording = Recordings.First(); recording; recording = Recordings.Next(recording)) {
#if APIVERSNUM >= 10705
        const cEvent* event = recording->Info()->GetEvent();
#else
        const cEvent* event = NULL;
#endif

        time_t recordingStart    = 0;
        int    recordingDuration = 0;

        if(event) {
            recordingStart    = event->StartTime();
            recordingDuration = event->Duration();
        }
        else {
            cRecordControl* rc = cRecordControls::GetRecordControl(recording->FileName());

            if(rc) {
                recordingStart    = rc->Timer()->StartTime();
                recordingDuration = rc->Timer()->StopTime() - recordingStart;
            }
            else {
#if APIVERSNUM >= 10727
                recordingStart = recording->Start();
#else
                recordingStart = recording->start;
#endif
            }
        }

        DEBUGLOG("GRI: RC: recordingStart=%lu recordingDuration=%i", recordingStart, recordingDuration);

        // recording_time
        m_resp->put_U32(recordingStart);

        // duration
        m_resp->put_U32(recordingDuration);

        // priority
        m_resp->put_U32(
#if APIVERSNUM >= 10727
            recording->Priority()
#else
            recording->priority
#endif
        );

        // lifetime
        m_resp->put_U32(
#if APIVERSNUM >= 10727
            recording->Lifetime()
#else
            recording->lifetime
#endif
        );

        // channel_name
        m_resp->put_String(recording->Info()->ChannelName() ? m_toUTF8.Convert(recording->Info()->ChannelName()) : "");

        char* fullname = strdup(recording->Name());
        char* recname = strrchr(fullname, FOLDERDELIMCHAR);
        char* directory = NULL;

        if(recname == NULL) {
            recname = fullname;
        }
        else {
            *recname = 0;
            recname++;
            directory = fullname;
        }

        // title
        m_resp->put_String(m_toUTF8.Convert(recname));

        // subtitle
        if(!isempty(recording->Info()->ShortText())) {
            m_resp->put_String(m_toUTF8.Convert(recording->Info()->ShortText()));
        }
        else {
            m_resp->put_String("");
        }

        // description
        if(!isempty(recording->Info()->Description())) {
            m_resp->put_String(m_toUTF8.Convert(recording->Info()->Description()));
        }
        else {
            m_resp->put_String("");
        }

        // directory
        if(directory != NULL) {
            char* p = directory;

            while(*p != 0) {
                if(*p == FOLDERDELIMCHAR) {
                    *p = '/';
                }

                if(*p == '_') {
                    *p = ' ';
                }

                p++;
            }

            while(*directory == '/') {
                directory++;
            }
        }

        m_resp->put_String((isempty(directory)) ? "" : m_toUTF8.Convert(directory));

        // filename / uid of recording
        uint32_t uid = RecordingsCache::GetInstance().Register(recording);
        char recid[9];
        snprintf(recid, sizeof(recid), "%08x", uid);
        m_resp->put_String(recid);

        // playcount
        m_resp->put_U32(reccache.GetPlayCount(uid));

        // content
        if(event != NULL) {
            m_resp->put_U32(event->Contents());
        }
        else {
            m_resp->put_U32(0);
        }

        // thumbnail url - for future use
        m_resp->put_String((const char*)reccache.GetPosterUrl(uid));

        // icon url - for future use
        m_resp->put_String((const char*)reccache.GetBackgroundUrl(uid));

        free(fullname);
    }

    m_resp->compress(m_compressionLevel);

    return true;
}

bool RoboTVClient::processRECORDINGS_Rename() { /* OPCODE 103 */
    uint32_t uid = 0;
    const char* recid = m_req->get_String();
    uid = recid2uid(recid);

    const char* newtitle     = m_req->get_String();
    cRecording* recording    = RecordingsCache::GetInstance().Lookup(uid);
    int         r            = ROBOTV_RET_DATAINVALID;

    if(recording != NULL) {
        // get filename and remove last part (recording time)
        char* filename_old = strdup((const char*)recording->FileName());
        char* sep = strrchr(filename_old, '/');

        if(sep != NULL) {
            *sep = 0;
        }

        // replace spaces in newtitle
        strreplace((char*)newtitle, ' ', '_');
        char filename_new[512];
        strncpy(filename_new, filename_old, 512);
        sep = strrchr(filename_new, '/');

        if(sep != NULL) {
            sep++;
            *sep = 0;
        }

        strncat(filename_new, newtitle, sizeof(filename_new) - 1);

        INFOLOG("renaming recording '%s' to '%s'", filename_old, filename_new);
        r = rename(filename_old, filename_new);
        Recordings.Update();

        free(filename_old);
    }

    m_resp->put_U32(r);

    return true;
}

bool RoboTVClient::processRECORDINGS_Delete() { /* OPCODE 104 */
    const char* recid = m_req->get_String();
    uint32_t uid = recid2uid(recid);
    cRecording* recording = RecordingsCache::GetInstance().Lookup(uid);

    if(recording == NULL) {
        ERRORLOG("Recording not found !");
        m_resp->put_U32(ROBOTV_RET_DATAUNKNOWN);
        return true;
    }

    DEBUGLOG("deleting recording: %s", recording->Name());

    cRecordControl* rc = cRecordControls::GetRecordControl(recording->FileName());

    if(rc != NULL) {
        ERRORLOG("Recording \"%s\" is in use by timer %d", recording->Name(), rc->Timer()->Index() + 1);
        m_resp->put_U32(ROBOTV_RET_DATALOCKED);
        return true;
    }

    if(!recording->Delete()) {
        ERRORLOG("Error while deleting recording!");
        m_resp->put_U32(ROBOTV_RET_ERROR);
        return true;
    }

    Recordings.DelByName(recording->FileName());
    INFOLOG("Recording \"%s\" deleted", recording->FileName());
    m_resp->put_U32(ROBOTV_RET_OK);

    return true;
}

bool RoboTVClient::processRECORDINGS_SetPlayCount() {
    const char* recid = m_req->get_String();
    uint32_t count = m_req->get_U32();

    uint32_t uid = recid2uid(recid);
    RecordingsCache::GetInstance().SetPlayCount(uid, count);

    return true;
}

bool RoboTVClient::processRECORDINGS_SetPosition() {
    const char* recid = m_req->get_String();
    uint64_t position = m_req->get_U64();

    uint32_t uid = recid2uid(recid);
    RecordingsCache::GetInstance().SetLastPlayedPosition(uid, position);

    return true;
}

bool RoboTVClient::processRECORDINGS_SetUrls() {
    const char* recid = m_req->get_String();
    const char* poster = m_req->get_String();
    const char* background = m_req->get_String();
    uint32_t id = m_req->get_U32();

    uint32_t uid = recid2uid(recid);
    RecordingsCache::GetInstance().SetPosterUrl(uid, poster);
    RecordingsCache::GetInstance().SetBackgroundUrl(uid, background);
    RecordingsCache::GetInstance().SetMovieID(uid, id);

    return true;
}

bool RoboTVClient::processArtwork_Get() {
    const char* title = m_req->get_String();
    uint32_t content = m_req->get_U32();

    std::string poster;
    std::string background;

    if(!m_artwork.get(content, title, poster, background)) {
        poster = "x";
        background = "x";
    }

    m_resp->put_String(poster.c_str());
    m_resp->put_String(background.c_str());
    m_resp->put_U32(0); // TODO - externalId

    return true;
}

bool RoboTVClient::processArtwork_Set() {
    const char* title = m_req->get_String();
    uint32_t content = m_req->get_U32();
    const char* poster = m_req->get_String();
    const char* background = m_req->get_String();
    uint32_t externalId = m_req->get_U32();

    INFOLOG("set artwork: %s (%i): %s", title, content, background);
    m_artwork.set(content, title, poster, background, externalId);
    return true;
}

bool RoboTVClient::processRECORDINGS_GetPosition() {
    const char* recid = m_req->get_String();

    uint32_t uid = recid2uid(recid);
    uint64_t position = RecordingsCache::GetInstance().GetLastPlayedPosition(uid);

    m_resp->put_U64(position);
    return true;
}

bool RoboTVClient::processRECORDINGS_GetMarks() {
#if VDRVERSNUM < 10732
    m_resp->put_U32(ROBOTV_RET_NOTSUPPORTED);
    return true;
#endif

    const char* recid = m_req->get_String();
    uint32_t uid = recid2uid(recid);

    cRecording* recording = RecordingsCache::GetInstance().Lookup(uid);

    if(recording == NULL) {
        ERRORLOG("GetMarks: recording not found !");
        m_resp->put_U32(ROBOTV_RET_DATAUNKNOWN);
        return true;
    }

    cMarks marks;

    if(!marks.Load(recording->FileName(), recording->FramesPerSecond(), recording->IsPesRecording())) {
        INFOLOG("no marks found for: '%s'", recording->FileName());
        m_resp->put_U32(ROBOTV_RET_NOTSUPPORTED);
        return true;
    }

    m_resp->put_U32(ROBOTV_RET_OK);

    m_resp->put_U64(recording->FramesPerSecond() * 10000);

#if VDRVERSNUM >= 10732

    cMark* end = NULL;
    cMark* begin = NULL;

    while((begin = marks.GetNextBegin(end)) != NULL) {
        end = marks.GetNextEnd(begin);

        if(end != NULL) {
            m_resp->put_String("SCENE");
            m_resp->put_U64(begin->Position());
            m_resp->put_U64(end->Position());
            m_resp->put_String(begin->ToText());
        }
    }

#endif

    return true;
}


/** OPCODE 120 - 139: RoboTV network functions for epg access and manipulating */

bool RoboTVClient::processEPG_GetForChannel() { /* OPCODE 120 */
    uint32_t channelUID = m_req->get_U32();
    uint32_t startTime  = m_req->get_U32();
    uint32_t duration   = m_req->get_U32();

    RoboTVChannels& c = RoboTVChannels::instance();
    c.Lock(false);

    const cChannel* channel = NULL;

    channel = FindChannelByUID(channelUID);

    if(channel != NULL) {
        DEBUGLOG("get schedule called for channel '%s'", (const char*)channel->GetChannelID().ToString());
    }

    if(!channel) {
        m_resp->put_U32(0);
        c.Unlock();

        ERRORLOG("written 0 because channel = NULL");
        return true;
    }

    cSchedulesLock MutexLock;
    const cSchedules* Schedules = cSchedules::Schedules(MutexLock);

    if(!Schedules) {
        m_resp->put_U32(0);
        c.Unlock();

        DEBUGLOG("written 0 because Schedule!s! = NULL");
        return true;
    }

    const cSchedule* Schedule = Schedules->GetSchedule(channel->GetChannelID());

    if(!Schedule) {
        m_resp->put_U32(0);
        c.Unlock();

        DEBUGLOG("written 0 because Schedule = NULL");
        return true;
    }

    bool atLeastOneEvent = false;

    uint32_t thisEventID;
    uint32_t thisEventTime;
    uint32_t thisEventDuration;
    uint32_t thisEventContent;
    uint32_t thisEventRating;
    const char* thisEventTitle;
    const char* thisEventSubTitle;
    const char* thisEventDescription;

    for(const cEvent* event = Schedule->Events()->First(); event; event = Schedule->Events()->Next(event)) {
        thisEventID           = event->EventID();
        thisEventTitle        = event->Title();
        thisEventSubTitle     = event->ShortText();
        thisEventDescription  = event->Description();
        thisEventTime         = event->StartTime();
        thisEventDuration     = event->Duration();
#if defined(USE_PARENTALRATING) || defined(PARENTALRATINGCONTENTVERSNUM)
        thisEventContent      = event->Contents();
        thisEventRating       = 0;
#elif APIVERSNUM >= 10711
        thisEventContent      = event->Contents();
        thisEventRating       = event->ParentalRating();
#else
        thisEventContent      = 0;
        thisEventRating       = 0;
#endif

        //in the past filter
        if((thisEventTime + thisEventDuration) < (uint32_t)time(NULL)) {
            continue;
        }

        //start time filter
        if((thisEventTime + thisEventDuration) <= startTime) {
            continue;
        }

        //duration filter
        if(duration != 0 && thisEventTime >= (startTime + duration)) {
            continue;
        }

        if(!thisEventTitle) {
            thisEventTitle        = "";
        }

        if(!thisEventSubTitle) {
            thisEventSubTitle     = "";
        }

        if(!thisEventDescription) {
            thisEventDescription  = "";
        }

        m_resp->put_U32(thisEventID);
        m_resp->put_U32(thisEventTime);
        m_resp->put_U32(thisEventDuration);
        m_resp->put_U32(thisEventContent);
        m_resp->put_U32(thisEventRating);

        m_resp->put_String(m_toUTF8.Convert(thisEventTitle));
        m_resp->put_String(m_toUTF8.Convert(thisEventSubTitle));
        m_resp->put_String(m_toUTF8.Convert(thisEventDescription));

        // add epg artwork
        if(m_protocolVersion >= 6) {
            std::string posterUrl;
            std::string backgroundUrl;

            if(m_artwork.get(thisEventContent, m_toUTF8.Convert(thisEventTitle), posterUrl, backgroundUrl)) {
                m_resp->put_String(posterUrl.c_str());
                m_resp->put_String(backgroundUrl.c_str());
            }
            else {
                m_resp->put_String("x");
                m_resp->put_String("x");
            }
        }

        atLeastOneEvent = true;
    }

    c.Unlock();
    DEBUGLOG("Got all event data");

    if(!atLeastOneEvent) {
        m_resp->put_U32(0);
        DEBUGLOG("Written 0 because no data");
    }

    m_resp->compress(m_compressionLevel);

    return true;
}


/** OPCODE 140 - 169: RoboTV network functions for channel scanning */

bool RoboTVClient::processSCAN_ScanSupported() { /* OPCODE 140 */
    if(m_scanSupported) {
        m_resp->put_U32(ROBOTV_RET_OK);
    }
    else {
        m_resp->put_U32(ROBOTV_RET_NOTSUPPORTED);
    }

    return true;
}

bool RoboTVClient::processSCAN_GetSetup() { /* OPCODE 141 */
    // get setup
    WIRBELSCAN_SERVICE::cWirbelscanScanSetup setup;

    if(!m_scanner.GetSetup(setup)) {
        INFOLOG("Unable to get wirbelscan setup !");
        m_resp->put_U32(ROBOTV_RET_NOTSUPPORTED);
        return true;
    }

    // get satellites
    cWirbelScan::List satellites;

    if(!m_scanner.GetSat(satellites)) {
        INFOLOG("Unable to get wirbelscan satellite list !");
        m_resp->put_U32(ROBOTV_RET_NOTSUPPORTED);
        return true;
    }

    // get coutries
    cWirbelScan::List countries;

    if(!m_scanner.GetCountry(countries)) {
        INFOLOG("Unable to get wirbelscan country list !");
        m_resp->put_U32(ROBOTV_RET_NOTSUPPORTED);
        return true;
    }

    // assemble response packet
    m_resp->put_U32(ROBOTV_RET_OK);

    // add setup
    m_resp->put_U16(setup.verbosity);
    m_resp->put_U16(setup.logFile);
    m_resp->put_U16(setup.DVB_Type);
    m_resp->put_U16(setup.DVBT_Inversion);
    m_resp->put_U16(setup.DVBC_Inversion);
    m_resp->put_U16(setup.DVBC_Symbolrate);
    m_resp->put_U16(setup.DVBC_QAM);
    m_resp->put_U16(setup.CountryId);
    m_resp->put_U16(setup.SatId);
    m_resp->put_U32(setup.scanflags);
    m_resp->put_U16(setup.ATSC_type);

    cCharSetConv toUTF8("ISO-8859-1", "UTF-8");

    // add satellites
    m_resp->put_U16(satellites.size());

    for(cWirbelScan::List::iterator i = satellites.begin(); i != satellites.end(); i++) {
        m_resp->put_S32(i->id);
        m_resp->put_String(toUTF8.Convert(i->short_name));
        m_resp->put_String(toUTF8.Convert(i->full_name));
    }

    // add countries
    m_resp->put_U16(countries.size());

    for(cWirbelScan::List::iterator i = countries.begin(); i != countries.end(); i++) {
        m_resp->put_S32(i->id);
        m_resp->put_String(toUTF8.Convert(i->short_name));
        m_resp->put_String(toUTF8.Convert(i->full_name));
    }

    m_resp->compress(m_compressionLevel);
    return true;
}

bool RoboTVClient::processSCAN_SetSetup() { /* OPCODE 141 */
    WIRBELSCAN_SERVICE::cWirbelscanScanSetup setup;

    // read setup
    setup.verbosity = m_req->get_U16();
    setup.logFile = m_req->get_U16();
    setup.DVB_Type = m_req->get_U16();
    setup.DVBT_Inversion = m_req->get_U16();
    setup.DVBC_Inversion = m_req->get_U16();
    setup.DVBC_Symbolrate = m_req->get_U16();
    setup.DVBC_QAM = m_req->get_U16();
    setup.CountryId = m_req->get_U16();
    setup.SatId = m_req->get_U16();
    setup.scanflags = m_req->get_U32();
    setup.ATSC_type = m_req->get_U16();

    INFOLOG("Logfile: %i", setup.logFile);

    // set setup
    if(!m_scanner.SetSetup(setup)) {
        INFOLOG("Unable to set wirbelscan setup !");
        m_resp->put_U32(ROBOTV_RET_NOTSUPPORTED);
        return true;
    }

    // store setup
    WIRBELSCAN_SERVICE::cWirbelscanCmd cmd;
    cmd.cmd = WIRBELSCAN_SERVICE::CmdStore;

    if(!m_scanner.DoCmd(cmd)) {
        INFOLOG("Unable to store wirbelscan setup !");
        m_resp->put_U32(ROBOTV_RET_NOTSUPPORTED);
        return true;
    }

    INFOLOG("new wirbelscan setup stored.");

    m_resp->put_U32(ROBOTV_RET_OK);
    return true;
}

bool RoboTVClient::processSCAN_Start() {
    WIRBELSCAN_SERVICE::cWirbelscanCmd cmd;
    cmd.cmd = WIRBELSCAN_SERVICE::CmdStartScan;

    if(!m_scanner.DoCmd(cmd)) {
        INFOLOG("Unable to start channel scanner !");
        m_resp->put_U32(ROBOTV_RET_NOTSUPPORTED);
        return true;
    }

    INFOLOG("channel scanner started ...");

    m_resp->put_U32(ROBOTV_RET_OK);
    return true;
}

bool RoboTVClient::processSCAN_Stop() {
    WIRBELSCAN_SERVICE::cWirbelscanCmd cmd;
    cmd.cmd = WIRBELSCAN_SERVICE::CmdStopScan;

    if(!m_scanner.DoCmd(cmd)) {
        INFOLOG("Unable to stop channel scanner !");
        m_resp->put_U32(ROBOTV_RET_NOTSUPPORTED);
        return true;
    }

    INFOLOG("channel scanner stopped.");

    m_resp->put_U32(ROBOTV_RET_OK);
    return true;
}

bool RoboTVClient::processSCAN_GetStatus() {
    WIRBELSCAN_SERVICE::cWirbelscanStatus status;

    if(!m_scanner.GetStatus(status)) {
        m_resp->put_U32(ROBOTV_RET_NOTSUPPORTED);
        return true;
    }

    m_resp->put_U32(ROBOTV_RET_OK);

    m_resp->put_U8((uint8_t)status.status);
    m_resp->put_U16(status.progress);
    m_resp->put_U16(status.strength);
    m_resp->put_U16(status.numChannels);
    m_resp->put_U16(status.newChannels);
    m_resp->put_String(status.curr_device);
    m_resp->put_String(status.transponder);

    m_resp->compress(m_compressionLevel);
    return true;
}

void RoboTVClient::SendScannerStatus() {
    WIRBELSCAN_SERVICE::cWirbelscanStatus status;

    if(!m_scanner.GetStatus(status)) {
        return;
    }

    MsgPacket* resp = new MsgPacket(ROBOTV_STATUS_CHANNELSCAN, ROBOTV_CHANNEL_STATUS);
    resp->setProtocolVersion(ROBOTV_PROTOCOLVERSION);

    resp->put_U8((uint8_t)status.status);
    resp->put_U16(status.progress);
    resp->put_U16(status.strength);
    resp->put_U16(status.numChannels);
    resp->put_U16(status.newChannels);
    resp->put_String(status.curr_device);
    resp->put_String(status.transponder);

    resp->compress(m_compressionLevel);

    QueueMessage(resp);
}

void RoboTVClient::QueueMessage(MsgPacket* p) {
    cMutexLock lock(&m_queueLock);
    m_queue.push(p);
}