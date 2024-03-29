#include "com_derek_live_JniPush_Pusher.h"
#include "string.h"
#include "stdlib.h"
#include "stdio.h"
#include <android/native_window.h>
#include <android/native_window_jni.h>

#include <pthread.h>
#include <android/log.h>
#include <faac/faac.h>
#include "x264/x264.h"
#include "rtmp/rtmp.h"
#include "queue.h"

#define LOGI(FORMAT,...) __android_log_print(ANDROID_LOG_INFO,"jason",FORMAT,##__VA_ARGS__);
#define LOGE(FORMAT,...) __android_log_print(ANDROID_LOG_ERROR,"jason",FORMAT,##__VA_ARGS__);

#define CONNECT_FAILED 101
#define INIT_FAILED 102
#define CONNECT_SUCCESS 103
#define INIT_SUCCESS 104
#define START_PUSH 105

x264_picture_t pic_in;
x264_picture_t pic_out;
x264_t * video_encoder_handle;

int y_len,u_len,v_len;

jobject jobj_push_native; //Global ref
jclass jcls_push_native;
jmethodID jmid_throw_native_error;

pthread_mutex_t mutex;
pthread_cond_t cond;

unsigned long nInputSamples; //输入的采样个数
unsigned long nMaxOutputBytes; //编码输出之后的字节数

int is_pushing = FALSE;

//faac音频编码处理器
faacEncHandle audio_encode_handle;

unsigned int start_time;

//rtmp流媒体地址
char *rtmp_path;

JavaVM *javaVM;

/**
 * 加入RTMPPacket队列，等待发送线程发送
 */
void add_rtmp_packet(RTMPPacket *packet){
    pthread_mutex_lock(&mutex);
    queue_append_last(packet);
    pthread_cond_signal(&cond);
    pthread_mutex_unlock(&mutex);
}

//获取JavaVM
jint JNI_OnLoad(JavaVM* vm, void* reserved){
    javaVM = vm;
    return JNI_VERSION_1_6;
}

/**
 * 向Java层发送错误信息
 */
void throwNativeError(JNIEnv *env,int code){
    if(jobj_push_native && jmid_throw_native_error )
        (*env)->CallVoidMethod(env,jobj_push_native,jmid_throw_native_error,code);
}


/**
 * 添加AAC头信息
 */
void add_aac_sequence_header(){
    //获取aac头信息的长度
    unsigned char *buf;
    unsigned long len; //长度
    faacEncGetDecoderSpecificInfo(audio_encode_handle,&buf,&len);
    int body_size = 2 + len;
    RTMPPacket *packet = malloc(sizeof(RTMPPacket));
    //RTMPPacket初始化
    RTMPPacket_Alloc(packet,body_size);
    RTMPPacket_Reset(packet);
    char * body = packet->m_body;
    //头信息配置
    /*AF 00 + AAC RAW data*/
    body[0] = 0xAF;//10 5 SoundFormat(4bits):10=AAC,SoundRate(2bits):3=44kHz,SoundSize(1bit):1=16-bit samples,SoundType(1bit):1=Stereo sound
    body[1] = 0x00;//AACPacketType:0表示AAC sequence header
    memcpy(&body[2], buf, len); /*spec_buf是AAC sequence header数据*/
    packet->m_packetType = RTMP_PACKET_TYPE_AUDIO;
    packet->m_nBodySize = body_size;
    packet->m_nChannel = 0x04;
    packet->m_hasAbsTimestamp = 0;
    packet->m_nTimeStamp = 0;
    packet->m_headerType = RTMP_PACKET_SIZE_MEDIUM;
    add_rtmp_packet(packet);
    free(buf);

}


/**
 * 添加AAC rtmp packet
 */
void add_aac_body(unsigned char *buf, int len){
    int body_size = 2 + len;
    RTMPPacket *packet = malloc(sizeof(RTMPPacket));
    //RTMPPacket初始化
    RTMPPacket_Alloc(packet,body_size);
    RTMPPacket_Reset(packet);
    char * body = packet->m_body;
    //头信息配置
    /*AF 00 + AAC RAW data*/
    body[0] = 0xAF;//10 5 SoundFormat(4bits):10=AAC,SoundRate(2bits):3=44kHz,SoundSize(1bit):1=16-bit samples,SoundType(1bit):1=Stereo sound
    body[1] = 0x01;//AACPacketType:1表示AAC raw
    memcpy(&body[2], buf, len); /*spec_buf是AAC raw数据*/
    packet->m_packetType = RTMP_PACKET_TYPE_AUDIO;
    packet->m_nBodySize = body_size;
    packet->m_nChannel = 0x04;
    packet->m_hasAbsTimestamp = 0;
    packet->m_headerType = RTMP_PACKET_SIZE_LARGE;
    packet->m_nTimeStamp = RTMP_GetTime() - start_time;
    add_rtmp_packet(packet);
}

/**
 * 从队列中不断拉取RTMPPacket发送给流媒体服务器）
 */
void *push_thread(void * arg){
    JNIEnv* env;//获取当前线程JNIEnv
    (*javaVM)->AttachCurrentThread(javaVM,&env,NULL);

    //建立RTMP连接
    RTMP *rtmp = RTMP_Alloc();
    if(!rtmp){
        LOGE("rtmp初始化失败");
        goto end;
    }
    RTMP_Init(rtmp);
    rtmp->Link.timeout = 5; //连接超时的时间
    //设置流媒体地址
    RTMP_SetupURL(rtmp,rtmp_path);
    //发布rtmp数据流
    RTMP_EnableWrite(rtmp);
    //建立连接
    if(!RTMP_Connect(rtmp,NULL)){
        LOGE("%s","RTMP 连接失败");
        goto end;
    }
    //计时
    start_time = RTMP_GetTime();
    if(!RTMP_ConnectStream(rtmp,0)){ //连接流
        throwNativeError(env,CONNECT_FAILED);
        goto end;
    }else{
        throwNativeError(env,CONNECT_SUCCESS);
    }
    is_pushing = TRUE;
    add_aac_sequence_header();

    while (is_pushing){
        //发送
        pthread_mutex_lock(&mutex);
        pthread_cond_wait(&cond,&mutex);
        //取出队列中的RTMPPacket
        RTMPPacket *packet = queue_get_first();
        if(packet){
            queue_delete_first(); //移除
            packet->m_nInfoField2 = rtmp->m_stream_id; //RTMP协议，stream_id数据
            int i = RTMP_SendPacket(rtmp,packet,TRUE); //TRUE放入librtmp队列中，并不是立即发送
            if(!i){
                LOGE("RTMP 断开");
                RTMPPacket_Free(packet);
                pthread_mutex_unlock(&mutex);
                goto end;
            }
            RTMPPacket_Free(packet);
        }

        pthread_mutex_unlock(&mutex);
    }
end:
    LOGI("%s","释放资源");
    free(rtmp_path);
    RTMP_Close(rtmp);
    RTMP_Free(rtmp);
    (*javaVM)->DetachCurrentThread(javaVM);
    return 0;
}

/*
 * Class:     com_derek_live_JniPush_Pusher
 * Method:    startPush
 * Signature: (Ljava/lang/String;)V
 */
JNIEXPORT void JNICALL Java_com_derek_live_JniPush_Pusher_startPush
        (JNIEnv * env, jobject jobj, jstring jstr_url){

    jobj_push_native = (*env)->NewGlobalRef(env,jobj);

    jclass jcls_push_native_tmp = (*env)->GetObjectClass(env,jobj);
    jcls_push_native = (*env)->NewGlobalRef(env,jcls_push_native_tmp);
    if(jcls_push_native_tmp == NULL){
        LOGI("%s","NULL");
    }else{
        LOGI("%s","not NULL");
    }
    //PushNative.throwNativeError
    jmid_throw_native_error = (*env)->GetMethodID(env,jcls_push_native_tmp,"throwNativeError","(I)V");

    //初始化的操作
    const char* url_cstr = (*env)->GetStringUTFChars(env,jstr_url,NULL);
    //复制url_cstr内容到rtmp_path
    rtmp_path = malloc(strlen(url_cstr) + 1);
    memset(rtmp_path,0,strlen(url_cstr) + 1);
    memcpy(rtmp_path,url_cstr,strlen(url_cstr));

    //初始化互斥锁与条件变量
    pthread_mutex_init(&mutex,NULL);
    pthread_cond_init(&cond,NULL);

    //创建队列
    create_queue();
    //启动消费者线程（从队列中不断拉取RTMPPacket发送给流媒体服务器）
    pthread_t push_thread_id;
    pthread_create(&push_thread_id, NULL,push_thread, NULL);

    (*env)->ReleaseStringUTFChars(env,jstr_url,url_cstr);
}

/*
 * Class:     com_derek_live_JniPush_Pusher
 * Method:    stopPush
 * Signature: ()V
 */
JNIEXPORT void JNICALL Java_com_derek_live_JniPush_Pusher_stopPush
        (JNIEnv * env, jobject obj){
    is_pushing = FALSE;
}

void releaseGlobalRef(JNIEnv* env,jobject gObj){
    if (gObj != NULL){
        (*env)->DeleteGlobalRef(env,gObj);
        gObj = NULL;
    }
}

/*
 * Class:     com_derek_live_JniPush_Pusher
 * Method:    release
 * Signature: ()V
 */
JNIEXPORT void JNICALL Java_com_derek_live_JniPush_Pusher_release
        (JNIEnv * env, jobject obj){
    LOGE("%s","jobj_push_native");
    releaseGlobalRef(env,jobj_push_native);
    LOGE("%s","jcls_push_native");
    releaseGlobalRef(env,jcls_push_native);
    LOGE("%s","end");
}

/*
 * Class:     com_derek_live_JniPush_Pusher
 * Method:    setVideoOptions
 * Signature: (IIII)V
 */
JNIEXPORT void JNICALL Java_com_derek_live_JniPush_Pusher_setVideoOptions
        (JNIEnv * env, jobject obj, jint width, jint height, jint bitrate, jint fps){
    x264_param_t param;
    //x264_param_default_preset
    x264_param_default_preset(&param,"ultrafast","zerolatency");

    param.i_csp = X264_CSP_I420;
    param.i_width = width;
    param.i_height = height;

    y_len = width * height;
    u_len = y_len/4;
    v_len = u_len;

//    i_rc_method 控制码率，CQP恒定质量，CRF恒定码率，ABR平均码率
    param.rc.i_rc_method = X264_RC_CRF;
    param.rc.i_bitrate = bitrate/1000;
    param.rc.i_vbv_max_bitrate = bitrate / 1000 * 1.2;

    //码率控制不通过timebase和timestamp,而是fps
    param.b_vfr_input = 0;
    param.i_fps_num = fps;
    param.i_fps_den = 1;
    //???????????????????????????
    param.i_timebase_den = param.i_fps_den;
    param.i_timebase_num = param.i_fps_num;
    param.i_threads = 1;//并行编码线程数量，0默认为多线程

    param.b_repeat_headers = 1;
    param.i_level_idc = 51;

    x264_param_apply_profile(&param,"baseline");


    x264_picture_alloc(&pic_in,param.i_csp,param.i_width,param.i_height);

    video_encoder_handle = x264_encoder_open(&param);
    if (video_encoder_handle){
        LOGI("%s","打开编码器成功。。。。。");
        throwNativeError(env,INIT_SUCCESS);
    }else{
        throwNativeError(env,INIT_FAILED);
    }

}

/*
 * Class:     com_derek_live_JniPush_Pusher
 * Method:    setAudioOptions
 * Signature: (II)V
 */
JNIEXPORT void JNICALL Java_com_derek_live_JniPush_Pusher_setAudioOptions
        (JNIEnv * env, jobject jobj, jint sampleRateInHz, jint numChannels){
    audio_encode_handle = faacEncOpen(sampleRateInHz,numChannels,&nInputSamples,&nMaxOutputBytes);
    if(!audio_encode_handle){
        LOGE("音频编码器打开失败");
        return;
    }
    //设置音频编码参数
    faacEncConfigurationPtr p_config = faacEncGetCurrentConfiguration(audio_encode_handle);
    p_config->mpegVersion = MPEG4;
    p_config->allowMidside = 1;
    p_config->aacObjectType = LOW;
    p_config->outputFormat = 0; //输出是否包含ADTS头
    p_config->useTns = 1; //时域噪音控制,大概就是消爆音
    p_config->useLfe = 0;
//	p_config->inputFormat = FAAC_INPUT_16BIT;
    p_config->quantqual = 100;
    p_config->bandWidth = 0; //频宽
    p_config->shortctl = SHORTCTL_NORMAL;

    if(!faacEncSetConfiguration(audio_encode_handle,p_config)){
        LOGE("%s","音频编码器配置失败..");
        throwNativeError(env,INIT_FAILED);
        return;
    }

    LOGI("%s","音频编码器配置成功");
}

void add_264_sequence_header(unsigned char* pps,unsigned char* sps,int pps_len,int sps_len){
    int body_size = 16 + sps_len + pps_len; //按照H264标准配置SPS和PPS，共使用了16字节
    RTMPPacket *packet = malloc(sizeof(RTMPPacket));
    //RTMPPacket初始化
    RTMPPacket_Alloc(packet,body_size);
    RTMPPacket_Reset(packet);

    char * body = packet->m_body;
    int i = 0;
    //二进制表示：00010111
    body[i++] = 0x17;//VideoHeaderTag:FrameType(1=key frame)+CodecID(7=AVC)
    body[i++] = 0x00;//AVCPacketType = 0表示设置AVCDecoderConfigurationRecord
    //composition time 0x000000 24bit ?
    body[i++] = 0x00;
    body[i++] = 0x00;
    body[i++] = 0x00;

    /*AVCDecoderConfigurationRecord*/
    body[i++] = 0x01;//configurationVersion，版本为1
    body[i++] = sps[1];//AVCProfileIndication
    body[i++] = sps[2];//profile_compatibility
    body[i++] = sps[3];//AVCLevelIndication
    //?
    body[i++] = 0xFF;//lengthSizeMinusOne,H264 视频中 NALU的长度，计算方法是 1 + (lengthSizeMinusOne & 3),实际测试时发现总为FF，计算结果为4.

    /*sps*/
    body[i++] = 0xE1;//numOfSequenceParameterSets:SPS的个数，计算方法是 numOfSequenceParameterSets & 0x1F,实际测试时发现总为E1，计算结果为1.
    body[i++] = (sps_len >> 8) & 0xff;//sequenceParameterSetLength:SPS的长度
    body[i++] = sps_len & 0xff;//sequenceParameterSetNALUnits
    memcpy(&body[i], sps, sps_len);
    i += sps_len;

    /*pps*/
    body[i++] = 0x01;//numOfPictureParameterSets:PPS 的个数,计算方法是 numOfPictureParameterSets & 0x1F,实际测试时发现总为E1，计算结果为1.
    body[i++] = (pps_len >> 8) & 0xff;//pictureParameterSetLength:PPS的长度
    body[i++] = (pps_len) & 0xff;//PPS
    memcpy(&body[i], pps, pps_len);
    i += pps_len;

    //Message Type，RTMP_PACKET_TYPE_VIDEO：0x09
    packet->m_packetType = RTMP_PACKET_TYPE_VIDEO;
    //Payload Length
    packet->m_nBodySize = body_size;
    //Time Stamp：4字节
    //记录了每一个tag相对于第一个tag（File Header）的相对时间。
    //以毫秒为单位。而File Header的time stamp永远为0。
    packet->m_nTimeStamp = 0;
    packet->m_hasAbsTimestamp = 0;
    packet->m_nChannel = 0x04; //Channel ID，Audio和Vidio通道
    packet->m_headerType = RTMP_PACKET_SIZE_MEDIUM; //?
    //将RTMPPacket加入队列
    add_rtmp_packet(packet);
}

void add_264_body(uint8_t * buf,int len){
//去掉起始码(界定符)
    if(buf[2] == 0x00){  //00 00 00 01
        buf += 4;
        len -= 4;
    }else if(buf[2] == 0x01){ // 00 00 01
        buf += 3;
        len -= 3;
    }
    int body_size = len + 9;
    RTMPPacket *packet = malloc(sizeof(RTMPPacket));
    RTMPPacket_Alloc(packet,body_size);

    unsigned char * body = packet->m_body;
    //当NAL头信息中，type（5位）等于5，说明这是关键帧NAL单元
    //buf[0] NAL Header与运算，获取type，根据type判断关键帧和普通帧
    //00000101 & 00011111(0x1f) = 00000101
    int type = buf[0] & 0x1f;
    //Inter Frame 帧间压缩
    body[0] = 0x27;//VideoHeaderTag:FrameType(2=Inter Frame)+CodecID(7=AVC)
    //IDR I帧图像
    if (type == NAL_SLICE_IDR) {
        body[0] = 0x17;//VideoHeaderTag:FrameType(1=key frame)+CodecID(7=AVC)
    }
    //AVCPacketType = 1
    body[1] = 0x01; /*nal unit,NALUs（AVCPacketType == 1)*/
    body[2] = 0x00; //composition time 0x000000 24bit
    body[3] = 0x00;
    body[4] = 0x00;

    //写入NALU信息，右移8位，一个字节的读取？
    body[5] = (len >> 24) & 0xff;
    body[6] = (len >> 16) & 0xff;
    body[7] = (len >> 8) & 0xff;
    body[8] = (len) & 0xff;

    /*copy data*/
    memcpy(&body[9], buf, len);

    packet->m_hasAbsTimestamp = 0;
    packet->m_nBodySize = body_size;
    packet->m_packetType = RTMP_PACKET_TYPE_VIDEO;//当前packet的类型：Video
    packet->m_nChannel = 0x04;
    packet->m_headerType = RTMP_PACKET_SIZE_LARGE;
//	packet->m_nTimeStamp = -1;
    packet->m_nTimeStamp = RTMP_GetTime() - start_time;//记录了每一个tag相对于第一个tag（File Header）的相对时间
    add_rtmp_packet(packet);

}


/*
 * Class:     com_derek_live_JniPush_Pusher
 * Method:    fireVideo
 * Signature: ([B)V
 */
JNIEXPORT void JNICALL Java_com_derek_live_JniPush_Pusher_fireVideo
        (JNIEnv * env, jobject obj, jbyteArray buffer){

    jbyte * nv21_buffer = (*env)->GetByteArrayElements(env,buffer,NULL);
    jbyte * u = pic_in.img.plane[1];
    jbyte * v = pic_in.img.plane[2];

    memcpy(pic_in.img.plane[0],nv21_buffer,y_len);
    int i = 0;
    for (int i = 0; i < u_len; ++i) {
        *(u+i) = *(nv21_buffer+y_len+i*2+1);
        *(v+i) = *(nv21_buffer+y_len+i*2);
    }

    x264_nal_t * nal = NULL;
    int n_nal = -1;
    if(x264_encoder_encode(video_encoder_handle,&nal,&n_nal,&pic_in,&pic_out) < 0){
        LOGE("%s","编码失败。。。。");
        return;
    }

//    使用rtmp将h264编码的视频数据传递到流媒体服务器
    //帧分为关键帧和普通帧，为了提高画面的纠错率，关键帧应包含PPS,SPS
    int sps_len,pps_len;
    unsigned char sps[100];
    unsigned char pps[100];

    memset(sps,0,100);
    memset(pps,0,100);

    for (int i = 0; i < n_nal; ++i) {
        if (nal[i].i_type == NAL_SPS){
            sps_len = nal[i].i_payload - 4;
            LOGI("sps_len %d",sps_len);
            memcpy(sps,nal[i].p_payload+4,(size_t)sps_len);

        }else if(nal[i].i_type == NAL_PPS){
            pps_len = nal[i].i_payload - 4;
            LOGI("pps_len %d",pps_len);
            memcpy(pps,nal[i].p_payload+4,(size_t)pps_len);

            add_264_sequence_header(pps,sps,pps_len,sps_len);
        }else{
            add_264_body(nal[i].p_payload,nal[i].i_payload);
        }
    }
    (*env)->ReleaseByteArrayElements(env,buffer,nv21_buffer,0);
}

/*
 * Class:     com_derek_live_JniPush_Pusher
 * Method:    fireAudio
 * Signature: ([BI)V
 */
JNIEXPORT void JNICALL Java_com_derek_live_JniPush_Pusher_fireAudio
        (JNIEnv * env, jobject obj, jbyteArray buffer, jint len){
    int *pcmbuf;
    unsigned char *bitbuf;
    jbyte* b_buffer = (*env)->GetByteArrayElements(env, buffer, 0);
    pcmbuf = (int*) malloc(nInputSamples * sizeof(int));
    bitbuf = (unsigned char*) malloc(nMaxOutputBytes * sizeof(unsigned char));
    int nByteCount = 0;
    unsigned int nBufferSize = (unsigned int) len / 2;
    unsigned short* buf = (unsigned short*) b_buffer;
    while (nByteCount < nBufferSize) {
        unsigned int audioLength = nInputSamples;
        if ((nByteCount + nInputSamples) >= nBufferSize) {
            audioLength = nBufferSize - nByteCount;
        }
        int i;
        for (i = 0; i < audioLength; i++) {//每次从实时的pcm音频队列中读出量化位数为8的pcm数据。
            int s = ((int16_t *) buf + nByteCount)[i];
            pcmbuf[i] = s << 8;//用8个二进制位来表示一个采样量化点（模数转换）
        }
        nByteCount += nInputSamples;
        //利用FAAC进行编码，pcmbuf为转换后的pcm流数据，audioLength为调用faacEncOpen时得到的输入采样数，bitbuf为编码后的数据buff，nMaxOutputBytes为调用faacEncOpen时得到的最大输出字节数
        int byteslen = faacEncEncode(audio_encode_handle, pcmbuf, audioLength,
                                     bitbuf, nMaxOutputBytes);
        if (byteslen < 1) {
            continue;
        }
        add_aac_body(bitbuf, byteslen);//从bitbuf中得到编码后的aac数据流，放到数据队列
    }
    (*env)->ReleaseByteArrayElements(env, buffer, b_buffer, 0);
    if (bitbuf)
        free(bitbuf);
    if (pcmbuf)
        free(pcmbuf);
}


