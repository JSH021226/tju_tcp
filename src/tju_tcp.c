
#include "tju_tcp.h"
#include "log.h"
#include "timer.h"
#include "syn.h"

/*
创建 TCP socket 
初始化对应的结构体
设置初始状态为 CLOSED
*/
tju_tcp_t* tju_socket(){

    // 初始化event log
    init_log();

    // 确定超时处理函数
    signal(SIGALRM, timeout_handler);

    tju_tcp_t* sock = (tju_tcp_t*)malloc(sizeof(tju_tcp_t));
    sock->state = CLOSED;
    
    // 初始化发送缓冲区
    pthread_mutex_init(&(sock->send_lock), NULL);
    sock->sending_buf = (char *)malloc(MAX_SOCK_BUF_SIZE);
    sock->sending_len = 0;
    sock->have_send_len=0;
    // 初始化接收缓冲区
    pthread_mutex_init(&(sock->recv_lock), NULL);
    sock->received_buf = (char *)malloc(MAX_SOCK_BUF_SIZE);
    sock->received_len = 0;

    if(pthread_cond_init(&sock->wait_cond, NULL) != 0){
        perror("ERROR condition variable not set\n");
        exit(-1);
    }

    // 初始化发送窗口
    sock->window.wnd_send = (sender_window_t*)malloc(sizeof(sender_window_t));
    sock->window.wnd_send->window_size=MAX_DLEN;    // 1 个MSS
    sock->window.wnd_send->ack_cnt=0;
    sock->window.wnd_send->base=1;
    sock->window.wnd_send->nextseq=1;
    sock->window.wnd_send->same_ack_cnt=0;
    sock->window.wnd_send->estmated_rtt=TCP_RTO_MIN;
    sock->window.wnd_send->dev_rtt=0;
    sock->window.wnd_send->is_estimating_rtt=FALSE;
    sock->window.wnd_send->rtt_expect_ack=0;  // (sending_thread线程中更新)
    // sock->window.wnd_send->send_time
    sock->window.wnd_send->timeout.it_value.tv_sec = 0;
    sock->window.wnd_send->timeout.it_value.tv_usec = 500000;
    sock->window.wnd_send->timeout.it_interval.tv_sec = 0;
    sock->window.wnd_send->timeout.it_interval.tv_usec = 0;
    sock->window.wnd_send->rwnd=MAX_RWINDOW_SIZE;
    sock->window.wnd_send->window_status=SLOW_START;
    sock->window.wnd_send->cwnd=MAX_DLEN;
    sock->window.wnd_send->ssthresh=MAX_SWINDOW_SIZE>>1;

    // 初始化接收窗口
    sock->window.wnd_recv = (receiver_window_t *)malloc(sizeof(receiver_window_t));
    sock->window.wnd_recv->remain_size=MAX_RWINDOW_SIZE;
    sock->window.wnd_recv->recv_buf=(char *)malloc(MAX_RWINDOW_SIZE);
    sock->window.wnd_recv->mark=(uint8_t *)malloc(MAX_RWINDOW_SIZE);
    memset(sock->window.wnd_recv->mark,0,MAX_RWINDOW_SIZE);
    sock->window.wnd_recv->expect_seq=1;

    sock->is_retransing=FALSE;

    sock->packet_FIN=NULL;

    return sock;
}

/*
绑定监听的地址 包括ip和端口
*/
int tju_bind(tju_tcp_t* sock, tju_sock_addr bind_addr){
    sock->bind_addr = bind_addr;
    return 0;
}

/*
被动打开 监听bind的地址和端口
设置socket的状态为LISTEN
注册该socket到内核的监听socket哈希表
*/
int tju_listen(tju_tcp_t* sock){
    // 初始化半连接和全连接队列
    init_queue();

    // 开启半连接队列维护线程(自动平台不能使用)
/*
    pthread_t id;
    int rst=pthread_create(&id,NULL,syn_retrans_thread,NULL);
    if (rst<0){
        printf("ERROR open 半连接队列维护线程\n");
        exit(-1);
    }
    printf("成功打开半连接队列维护线程\n");
*/

    sock->state = LISTEN;
    int hashval = cal_hash(sock->bind_addr.ip, sock->bind_addr.port, 0, 0);
    listen_socks[hashval] = sock;
    return 0;
}

/*
接受连接 
返回与客户端通信用的socket
这里返回的socket一定是已经完成3次握手建立了连接的socket
因为只要该函数返回, 用户就可以马上使用该socket进行send和recv
*/
tju_tcp_t* tju_accept(tju_tcp_t* listen_sock){
    
    // 判断全连接队列中是否有 socket
    printf("开始监听全连接队列\n");
    tju_tcp_t* accept_socket=get_from_accept();     // 队列为空 阻塞
    printf("从全连接队列中取出一个sock\n");

    tju_tcp_t* new_conn = accept_socket;
    if (new_conn==NULL){
        printf("accpet取出失败\n");
        exit(-1);
    }

    // 将新的conn放到内核建立连接的socket哈希表中
    int hashval = cal_hash(new_conn->established_local_addr.ip, new_conn->established_local_addr.port, \
                new_conn->established_remote_addr.ip, new_conn->established_remote_addr.port);
    established_socks[hashval] = new_conn;

    // 如果new_conn的创建过程放到了tju_handle_packet中 那么accept怎么拿到这个new_conn呢
    // 在linux中 每个listen socket都维护一个已经完成连接的socket队列
    // 每次调用accept 实际上就是取出这个队列中的一个元素
    // 队列为空,则阻塞 
    printf("服务器三次握手完成\n");

    // 创建发送线程
    pthread_t sending_thread_id=555;
    int rst1=pthread_create(&sending_thread_id,NULL,sending_thread,(void *)new_conn);
    if (rst1<0){
        printf("sending thread 创建失败\n");
        exit(-1);
    }
    printf("sending thread 创建成功\n");

    // 创建重传线程
    pthread_t retrans_thread_id=556;
    int rst2=pthread_create(&retrans_thread_id,NULL,retrans_thread,(void *)new_conn);
    if (rst2<0){
        printf("retrans thread 创建失败\n");
        exit(-1);
    }
    printf("retrans thread 创建成功\n");
    return new_conn;
}

/*
连接到服务端
该函数以一个socket为参数
调用函数前, 该socket还未建立连接
函数正常返回后, 该socket一定是已经完成了3次握手, 建立了连接
因为只要该函数返回, 用户就可以马上使用该socket进行send和recv
*/
int tju_connect(tju_tcp_t* sock, tju_sock_addr target_addr){

    // 将socket绑定本地地址
    clock_t time_point;     // 计时用
    tju_sock_addr local_addr;
    local_addr.ip = inet_network(CLIENT_IP);
    local_addr.port = 5678; // 连接方进行connect连接的时候 内核中是随机分配一个可用的端口
    sock->established_local_addr = local_addr;

    // 向客户端发送 SYN 报文，并将状态改为 SYN_SENT
    uint32_t seq=CLIENT_ISN;
    char* packet_SYN=create_packet_buf(local_addr.port,target_addr.port,seq,0,\
            DEFAULT_HEADER_LEN,DEFAULT_HEADER_LEN,SYN_FLAG_MASK,1,0,NULL,0);
    sendToLayer3(packet_SYN,DEFAULT_HEADER_LEN);
    _SEND_LOG_(packet_SYN);
    time_point=clock();  // 开始计时
    sock->state=SYN_SENT;
    printf("客户端发送SYN---第一次握手\n");

    // 将即将建立连接的socket放入内核 已建立连接哈希表中
    int hashval = cal_hash(local_addr.ip, local_addr.port, target_addr.ip, target_addr.port);
    established_socks[hashval] = sock;

    // 阻塞等待（简单计时器）
    while (sock->state!=ESTABLISHED){
        if ((clock()-time_point)>=6000000){    //触发计时器--超时重传   (CLOCK_PER_SEC)
            sendToLayer3(packet_SYN,DEFAULT_HEADER_LEN);
            _SEND_LOG_(packet_SYN);
            printf("客户端重新发送SYN请求----第一次握手\n");
            time_point=clock(); //重新开始计时
        }
    }
    
    // 三次握手完成
    sock->established_remote_addr = target_addr;    // 绑定远端地址

    free(packet_SYN);
    
    printf("客户端三次握手完成\n");

    // 创建发送线程
    pthread_t sending_thread_id=557;
    int rst1=pthread_create(&sending_thread_id,NULL,sending_thread,(void *)sock);
    if (rst1<0){
        printf("sending thread 创建失败\n");
        exit(-1);
    }
    printf("sending thread 创建成功\n");

    // 创建重传线程
    pthread_t retrans_thread_id=558;
    int rst2=pthread_create(&retrans_thread_id,NULL,retrans_thread,(void *)sock);
    if (rst2<0){
        printf("retrans thread 创建失败\n");
        exit(-1);
    }
    printf("retrans thread 创建成功\n");
    return 0;
}

// 阻塞执行，负责将数据拷贝至缓冲区
int tju_send(tju_tcp_t* sock, const void *buffer, int len){
    char* tmp_buffer=(char *)buffer;    // 当前待拷贝数据指针
    int tmp_len=len;    // 剩余待拷贝数据长度
    while (MAX_SOCK_BUF_SIZE-sock->sending_len>0||tmp_len){
        if (MAX_SOCK_BUF_SIZE-sock->sending_len==0) continue;
        pthread_mutex_lock(&sock->send_lock);   // 加锁
        if (MAX_SOCK_BUF_SIZE-sock->sending_len>=tmp_len){  // 剩余缓冲区空间足够存放待发送数据
            // 拷贝数据至缓冲区
            memcpy(sock->sending_buf+sock->sending_len,tmp_buffer,tmp_len);
            sock->sending_len+=tmp_len;
            pthread_mutex_unlock(&sock->send_lock); // 解锁
            break;
        }
        else{   // 缓冲区空间不够存放待发送数据
            memcpy(sock->sending_buf+sock->sending_len,tmp_buffer,MAX_SOCK_BUF_SIZE-sock->sending_len);
            sock->sending_len=MAX_SOCK_BUF_SIZE;
            tmp_buffer+=MAX_SOCK_BUF_SIZE-sock->sending_len;
            tmp_len-=MAX_SOCK_BUF_SIZE-sock->sending_len;
            pthread_mutex_unlock(&sock->send_lock); // 解锁
        }
    }

    return 0;
}
int tju_recv(tju_tcp_t* sock, void *buffer, int len){
    // printf("调用tju_recv函数\n");
    while(sock->received_len<=0){
        // 阻塞
    }

    while(pthread_mutex_lock(&(sock->recv_lock)) != 0); // 加锁

    int read_len = 0;
    if (sock->received_len >= len){ // 从中读取len长度的数据
        read_len = len;
    }else{
        read_len = sock->received_len; // 读取sock->received_len长度的数据(全读出来)
    }

    memcpy(buffer, sock->received_buf, read_len);

    if(read_len < sock->received_len) { // 还剩下一些
        char* new_buf =(char *)malloc(MAX_SOCK_BUF_SIZE);
        memcpy(new_buf, sock->received_buf + read_len, sock->received_len - read_len);
        free(sock->received_buf);
        sock->received_len -= read_len;
        sock->received_buf = new_buf;
    }else{
        sock->received_len = 0;
    }
    pthread_mutex_unlock(&(sock->recv_lock)); // 解锁

    // for (int i=0;i<100000;i++) ; // 降速用

    return read_len;
}

int tju_handle_packet(tju_tcp_t* sock, char* pkt){
    _RECV_LOG_(pkt);
    // 判断收到报文的 socket 的状态是否为 SYN_SENT
    if (sock->state==SYN_SENT){
        if (get_ack(pkt)==CLIENT_ISN+1){
            char* packet_SYN_ACK2=create_packet_buf(get_dst(pkt),get_src(pkt),get_ack(pkt),get_seq(pkt)+1,\
                        DEFAULT_HEADER_LEN,DEFAULT_HEADER_LEN,ACK_FLAG_MASK,1,0,NULL,0);
            sendToLayer3(packet_SYN_ACK2,DEFAULT_HEADER_LEN);
            _SEND_LOG_(packet_SYN_ACK2);
            free(packet_SYN_ACK2);
            sock->state=ESTABLISHED;
            printf("客户端发送SYN_ACK----第三次握手\n");
        }
    }
    else if (sock->state==LISTEN){
        if (get_flags(pkt)==SYN_FLAG_MASK){
            // 向客户端发送 SYN_ACK 报文
            char* packet_SYN_ACK1=create_packet_buf(get_dst(pkt),get_src(pkt),SERVER_ISN,get_seq(pkt)+1,\
                        DEFAULT_HEADER_LEN,DEFAULT_HEADER_LEN,(SYN_FLAG_MASK|ACK_FLAG_MASK),1,0,NULL,0);
            sendToLayer3(packet_SYN_ACK1,DEFAULT_HEADER_LEN);
            _SEND_LOG_(packet_SYN_ACK1);
            printf("服务器发送SYN_ACK----第二次握手\n");

            // 将 socket 存入半连接队列中
            tju_tcp_t* new_conn = (tju_tcp_t*)malloc(sizeof(tju_tcp_t));
            memcpy(new_conn, sock, sizeof(tju_tcp_t));
            // 绑定地址
            new_conn->established_local_addr=new_conn->bind_addr;
            new_conn->established_remote_addr.ip=inet_network(CLIENT_IP);
            new_conn->established_remote_addr.port=get_src(pkt);

            new_conn->state=SYN_RECV;
            
            // 该sock加入半连接队列
            en_syn_queue(new_conn,packet_SYN_ACK1);
            printf("sock进半连接队列\n");
        }
        else if (get_flags(pkt)==ACK_FLAG_MASK&&get_ack(pkt)==SERVER_ISN+1){
            // 取出半连接中的socket加入全连接队列中
            tju_tcp_t* tmp_conn=get_from_syn(pkt);
            printf("从半连接队列中取出sock\n");
            if (tmp_conn==NULL){
                printf("半连接队列中已不存在该sock\n");
                return 0;
            }

            tmp_conn->state=ESTABLISHED;

            en_accept_queue(tmp_conn);
            printf("sock加入全连接队列\n");
        }
    }
    else if (sock->state==ESTABLISHED){
        // 服务器收到FIN报文
        if (get_flags(pkt)==(FIN_FLAG_MASK)){     
            // 发送FIN_ACK报文
            uint32_t ack=get_seq(pkt)+1;
            char* packet_FIN_ACK=create_packet_buf(get_dst(pkt),get_src(pkt),get_ack(pkt),ack,\
                    DEFAULT_HEADER_LEN,DEFAULT_HEADER_LEN,ACK_FLAG_MASK,1,0,NULL,0);
            sendToLayer3(packet_FIN_ACK,DEFAULT_HEADER_LEN);
            _SEND_LOG_(packet_FIN_ACK);
            free(packet_FIN_ACK);
            printf("服务器发送ACK报文并进入CLOSE_WAIT\n");

            // 更新状态
            sock->state=CLOSE_WAIT;
            sleep(1);          // 等待，防止混入 同时关闭 的情况

            // 服务器发送第三次挥手FIN_ACK包
            char* packet_FIN=create_packet_buf(sock->established_local_addr.port,sock->established_remote_addr.port,FIN_SEQ,\
                            ack,DEFAULT_HEADER_LEN,DEFAULT_HEADER_LEN,FIN_FLAG_MASK|ACK_FLAG_MASK,1,0,NULL,0);
            sendToLayer3(packet_FIN,DEFAULT_HEADER_LEN);
            _SEND_LOG_(packet_FIN);
            sock->packet_FIN=packet_FIN;
            sock->state=LAST_ACK;
            printf("服务器完成第三次挥手，进入LAST_ACK\n");

            // 服务器创建线程等待关闭sock
            pthread_t id=559;
            int rst=pthread_create(&id,NULL,tju_close_thread,(void *)sock);
            if (rst<0){
                printf("ERROR open tju_close_thread\n");
                exit(-1); 
            }
        }

        // 收到数据报文
        else if (get_flags(pkt)==NO_FLAG){      
            // 判断收到的报文序列号是否为期望的序号
            // printf("缓冲区剩余：%d\n",MAX_SOCK_BUF_SIZE-sock->received_len);
            // printf("收到seq = %d 的报文  发送ACK报文 ack = %d\n", get_seq(pkt), sock->window.wnd_recv->expect_seq);
            if (get_seq(pkt)>=sock->window.wnd_recv->expect_seq){
                // 放入接收窗口缓冲区
                uint16_t dlen=get_plen(pkt)-get_hlen(pkt);
                uint32_t expt_seq=sock->window.wnd_recv->expect_seq;
                char* recv_buf=sock->window.wnd_recv->recv_buf;
                uint8_t* mark=sock->window.wnd_recv->mark;
                if (get_seq(pkt)+dlen<expt_seq+MAX_RWINDOW_SIZE){
                    // 接收窗口可以放下
                    memcpy(recv_buf+get_seq(pkt)-expt_seq,pkt+get_hlen(pkt),dlen);
                    memset(mark+get_seq(pkt)-expt_seq,1,dlen);
                }

                // 窗口可以前移
                if (mark[0]!=0){
                    uint16_t free_len=get_wnd_free_len(mark);
                    if (MAX_SOCK_BUF_SIZE-sock->received_len<free_len){
                        // printf("接收缓冲区装不下，丢弃该报文\n");
                        // 更新接收窗口剩余大小
                        sock->window.wnd_recv->remain_size=MAX_RWINDOW_SIZE-free_len;
                    }
                    else{
                        sock->window.wnd_recv->remain_size=MAX_RWINDOW_SIZE;
                        // 缓冲区能够装下该数据报
                        pthread_mutex_lock(&(sock->recv_lock));     // 加锁
                        memcpy(sock->received_buf+sock->received_len,recv_buf,free_len);
                        sock->received_len+=free_len;
                        pthread_mutex_unlock(&(sock->recv_lock)); // 解锁

                        // 更新接收窗口
                        sock->window.wnd_recv->recv_buf=(char *)malloc(MAX_RWINDOW_SIZE);
                        memcpy(sock->window.wnd_recv->recv_buf,recv_buf+free_len,MAX_RWINDOW_SIZE-free_len);
                        free(recv_buf);
                        sock->window.wnd_recv->mark=(uint8_t *)malloc(MAX_RWINDOW_SIZE);
                        memset(sock->window.wnd_recv->mark,0,MAX_RWINDOW_SIZE);
                        memcpy(sock->window.wnd_recv->mark,mark+free_len,MAX_RWINDOW_SIZE-free_len);
                        free(mark);
                        sock->window.wnd_recv->expect_seq+=free_len;

                        _DELV_LOG_(expt_seq,free_len);
                    }

                }

                // 发送ACK报文
                uint32_t seq=sock->window.wnd_send->nextseq;
                uint32_t ack = sock->window.wnd_recv->expect_seq;
                uint16_t adv_window = sock->window.wnd_recv->remain_size;

                char* tmp_packet_ACK1=create_packet_buf(sock->established_local_addr.port,sock->established_remote_addr.port,seq,ack,\
                        DEFAULT_HEADER_LEN,DEFAULT_HEADER_LEN,ACK_FLAG_MASK,adv_window,0,NULL,0);
                sendToLayer3(tmp_packet_ACK1,DEFAULT_HEADER_LEN);
                _SEND_LOG_(tmp_packet_ACK1);
                free(tmp_packet_ACK1);
            }
            else{   // 收到的序列号不是所期望的
                // 直接发送ACK报文
                uint32_t seq=sock->window.wnd_send->nextseq;
                uint32_t ack = sock->window.wnd_recv->expect_seq;
                uint16_t adv_window = sock->window.wnd_recv->remain_size;

                char* tmp_packet_ACK2=create_packet_buf(sock->established_local_addr.port,sock->established_remote_addr.port,seq,ack,\
                        DEFAULT_HEADER_LEN,DEFAULT_HEADER_LEN,ACK_FLAG_MASK,adv_window,0,NULL,0);
                sendToLayer3(tmp_packet_ACK2,DEFAULT_HEADER_LEN);
                _SEND_LOG_(tmp_packet_ACK2);
                free(tmp_packet_ACK2);
            }
        }

        // 收到ACK报文
        else if (get_flags(pkt)==ACK_FLAG_MASK){

            // 更新流量控制窗口
            sock->window.wnd_send->rwnd=get_advertised_window(pkt);
            _RWND_LOG_(sock);

            // 如果收到的 ack 在窗口外则直接丢掉
            if (get_ack(pkt) < sock->window.wnd_send->base){
                printf("收到的ack报文在发送窗口外 丢弃报文 \n");

                // 更新发送窗口
                sock->window.wnd_send->window_size=min(sock->window.wnd_send->cwnd,sock->window.wnd_send->rwnd);
                _SWND_LOG_(sock);
            }
            // 收到重复的ack
            else if (get_ack(pkt) == sock->window.wnd_send->base){
                printf("收到重复ACK报文 ack=%d\n", get_ack(pkt));

                // 慢启动 & 拥塞避免
                if (sock->window.wnd_send->window_status==SLOW_START||sock->window.wnd_send->window_status==CONGESTION_AVOIDANCE){
                    sock->window.wnd_send->same_ack_cnt++;
                }
                // 快速恢复
                else if (sock->window.wnd_send->window_status==FAST_RECOVERY){
                    sock->window.wnd_send->cwnd+=MAX_DLEN;
                    _CWND_LOG_(sock,FAST_RECOVERY);
                }

                // 连续收到 3 个重复的ack
                if (sock->window.wnd_send->same_ack_cnt==3&&sock->window.wnd_send->window_status!=FAST_RECOVERY){
                    // 拥塞阈值更新为发送窗口的一半
                    sock->window.wnd_send->ssthresh=sock->window.wnd_send->cwnd>>1;
                    // 当前拥塞窗口更新为拥塞阈值
                    sock->window.wnd_send->cwnd=sock->window.wnd_send->ssthresh;
                    _CWND_LOG_(sock,FAST_RECOVERY);
                    // 更新发送窗口状态为 快速恢复
                    sock->window.wnd_send->window_status=FAST_RECOVERY;

                    printf("收到三个重复ack，开始快速重传\n");
                    // 开启快速重传
                    sock->is_retransing = true;
                    RETRANS=TRUE;
                }

                // 更新发送窗口
                sock->window.wnd_send->window_size=min(sock->window.wnd_send->cwnd,sock->window.wnd_send->rwnd);
                _SWND_LOG_(sock);
            }
            // 收到可用于更新的ACK
            else{
                printf("收到有效ACK报文 ack=%d\n", get_ack(pkt));

                sock->window.wnd_send->same_ack_cnt=0;  // 刷新快速重传计数

                // 更新拥塞窗口大小、状态
                if (sock->window.wnd_send->window_status==SLOW_START){
                    sock->window.wnd_send->cwnd*=2;
                    if (sock->window.wnd_send->cwnd>=sock->window.wnd_send->ssthresh){
                        sock->window.wnd_send->cwnd=sock->window.wnd_send->ssthresh;

                        sock->window.wnd_send->window_status=CONGESTION_AVOIDANCE;
                    }
                    _CWND_LOG_(sock,SLOW_START);
                }
                else if (sock->window.wnd_send->window_status==CONGESTION_AVOIDANCE){
                    sock->window.wnd_send->cwnd+=MAX_DLEN;
                    _CWND_LOG_(sock,CONGESTION_AVOIDANCE);
                }
                else if (sock->window.wnd_send->window_status==FAST_RECOVERY){
                    sock->window.wnd_send->window_status=CONGESTION_AVOIDANCE;
                }

                // 更新发送窗口
                sock->window.wnd_send->window_size=min(sock->window.wnd_send->cwnd,sock->window.wnd_send->rwnd);
                _SWND_LOG_(sock);

                // 开始计算 SampleRTT
                if (sock->window.wnd_send->is_estimating_rtt==TRUE){
                    if (sock->window.wnd_send->rtt_expect_ack==get_ack(pkt)){
                        CalTimeout(sock);
                    }
                    sock->window.wnd_send->is_estimating_rtt=FALSE;
                }

                // 窗口滑动
                sock->window.wnd_send->ack_cnt+=get_ack(pkt)-sock->window.wnd_send->base;
                sock->window.wnd_send->base = get_ack(pkt);

                // 启动定时器
                if (sock->window.wnd_send->base == sock->window.wnd_send->nextseq){
                    // 缓冲区中没有尚未发送的数据
                    stopTimer();
                    sock->is_retransing=FALSE;
                }
                else{
                    // 重新开始计时
                    startTimer(sock);
                    sock->is_retransing=TRUE;
                }
                
                // 清理发送缓冲区中已收到确认的数据
                if (sock->sending_len&&(sock->window.wnd_send->ack_cnt==sock->sending_len || sock->window.wnd_send->ack_cnt>4*MAX_SOCK_BUF_SIZE/5))
                {
                    // printf("正在清理发送缓冲区:\n");
                    pthread_mutex_lock(&sock->send_lock);   // 加锁
                    char* new_sending_buf=(char *)malloc(MAX_SOCK_BUF_SIZE);
                    memcpy(new_sending_buf,sock->sending_buf+sock->window.wnd_send->ack_cnt,sock->sending_len-sock->window.wnd_send->ack_cnt);
                    free(sock->sending_buf);
                    sock->sending_buf=new_sending_buf;
                    sock->sending_len=sock->sending_len-sock->window.wnd_send->ack_cnt;
                    sock->have_send_len=sock->have_send_len-sock->window.wnd_send->ack_cnt;
                    sock->window.wnd_send->ack_cnt=0;
                    pthread_mutex_unlock(&sock->send_lock);  // 解锁
                    // printf("清理完毕\n");
                }
            }

            // 0 窗口开启计时器
            if (sock->window.wnd_send->window_size==0){
                printf("开启0窗口定时器: %d  %d\n",sock->window.wnd_send->cwnd,sock->window.wnd_send->rwnd);
                startTimer(sock);
                sock->is_retransing=FALSE;      // 表示当前为 0 窗口探测阶段
            }
        }

        // 服务器syn队列支持超时重传时使用
        /*
        else if ((get_flags(pkt)==(SYN_FLAG_MASK|ACK_FLAG_MASK))&&(get_ack(pkt)==CLIENT_ISN+1)){    // 客户端收到SYN_ACK报文，说明这是服务器syn队列重传的
            char* packet_SYN_ACK3=create_packet_buf(get_dst(pkt),get_src(pkt),get_ack(pkt),get_seq(pkt)+1,\
                        DEFAULT_HEADER_LEN,DEFAULT_HEADER_LEN,ACK_FLAG_MASK,1,0,NULL,0);
            sendToLayer3(packet_SYN_ACK3,DEFAULT_HEADER_LEN);
            printf("客户端已建立连接，收到SYN_ACK\n");
        }
            */

    }
    else if (sock->state==FIN_WAIT_1){
        if (get_flags(pkt)==ACK_FLAG_MASK&&get_ack(pkt)==FIN_SEQ+1){    // 双方先后关闭
            sock->state=FIN_WAIT_2;
            printf("客户端进入FIN_WAIT_2\n");
        }
        else if (get_flags(pkt)==(FIN_FLAG_MASK|ACK_FLAG_MASK)||(get_flags(pkt)==FIN_FLAG_MASK)){    // 同时关闭
            // 发送FIN_ACK报文
            char* packet_FIN_ACK3=create_packet_buf(get_dst(pkt),get_src(pkt),FIN_SEQ+1,get_seq(pkt)+1,\
                    DEFAULT_HEADER_LEN,DEFAULT_HEADER_LEN,ACK_FLAG_MASK,1,0,NULL,0);
            sendToLayer3(packet_FIN_ACK3,DEFAULT_HEADER_LEN);
            _SEND_LOG_(packet_FIN_ACK3);
            free(packet_FIN_ACK3);
            // 状态更新
            sock->state=CLOSING;
            printf("同时关闭进入CLOSING\n");
        }
    }
    else if (sock->state==FIN_WAIT_2){
        if (get_flags(pkt)==(FIN_FLAG_MASK|ACK_FLAG_MASK)){
            // 发送FIN_ACK报文
            char* packet_FIN_ACK2=create_packet_buf(get_dst(pkt),get_src(pkt),get_ack(pkt),get_seq(pkt)+1,\
                    DEFAULT_HEADER_LEN,DEFAULT_HEADER_LEN,ACK_FLAG_MASK,1,0,NULL,0);
            sendToLayer3(packet_FIN_ACK2,DEFAULT_HEADER_LEN);
            _SEND_LOG_(packet_FIN_ACK2);
            free(packet_FIN_ACK2);

            // 更新socket状态并等待2MSL
            sock->state=TIME_WAIT;
            printf("客户端发送ACK报文并进入TIME_WAIT，并等待2MSL\n");
            sleep(3);
            sock->state=CLOSED;
        }
    }
    else if (sock->state==LAST_ACK){
        if (get_flags(pkt)==ACK_FLAG_MASK&&get_ack(pkt)==FIN_SEQ+1){
            printf("服务器最后确认完成--LAST_ACK\n");
            sock->state=CLOSED;
        }
    }
    else if (sock->state==CLOSING){
        if (get_flags(pkt)==ACK_FLAG_MASK&&get_ack(pkt)==FIN_SEQ+1){
            sock->state=TIME_WAIT;
            printf("同时关闭---进如TIME_WAIT等待2MSL\n");
            sleep(3);          // 等待2MSL
            sock->state=CLOSED;
        }
    }

    return 0;
}

int tju_close (tju_tcp_t* sock){
    clock_t time_point;     //计时用
    if (sock->state==ESTABLISHED){      // 主动调用
        // FIN报文
        char* packet_FIN=create_packet_buf(sock->established_local_addr.port,sock->established_remote_addr.port,FIN_SEQ,\
                        0,DEFAULT_HEADER_LEN,DEFAULT_HEADER_LEN,FIN_FLAG_MASK,1,0,NULL,0);
        // 发送
        sendToLayer3(packet_FIN,DEFAULT_HEADER_LEN);
        _SEND_LOG_(packet_FIN);
        sock->packet_FIN=packet_FIN;
        time_point=clock();     // 开始计时
        // 状态更新
        sock->state=FIN_WAIT_1;
        printf("客户端进入FIN_WAIT_1\n");
    }
    else if (sock->state==LAST_ACK){    // 被动调用
        time_point=clock(); // 开始计时
    }
    else{
        printf("关闭sock时状态错误\n");
        exit(-1);
    }

    // 阻塞等待（支持超时重传）
    printf("等待中......\n");
    while (sock->state!=CLOSED){
        if ((clock()-time_point)>=8000000){
            sendToLayer3(sock->packet_FIN,DEFAULT_HEADER_LEN);
            _SEND_LOG_(sock->packet_FIN);
            printf("超时重传FIN\n");
            time_point=clock();
        }
    }

    printf("连接关闭\n");
    // 释放资源
    int hashval=cal_hash(sock->established_local_addr.ip,sock->established_local_addr.port,\
                sock->established_remote_addr.ip,sock->established_remote_addr.port);
    established_socks[hashval]=NULL;
    free(sock->packet_FIN);
    free(sock);

    // 关闭event log
    close_log();

    return 0;
}

/*
    以下是自定义辅助函数
*/
uint16_t get_wnd_free_len(uint8_t* mark){   // 获取窗口前移长度
    uint16_t ans=0;
    while (mark[ans]!=0){
        ans++;
    }
    return ans;
}

