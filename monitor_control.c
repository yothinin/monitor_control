#include <gpiod.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include "oled_i2c.h"

#define DEBOUNCE_DELAY_US 5000
#define DEBOUNCE_COUNT 5
#define MAX_MONITORS 3
#define NUM_KEYS 3
#define FONT_PATH "./fonts/NotoSerifThai.ttf"
#define FONT_SIZE 24
#define PING_TIMEOUT_US 500000  // 0.5s

// GPIO
static struct gpiod_chip *chip;
static struct gpiod_line *do_sw;
static struct gpiod_line *btn_down;
static struct gpiod_line *btn_up;
static struct gpiod_line *btn_done;
static struct gpiod_line *monitor_keys[NUM_KEYS];

// GPIO LED
static struct gpiod_line *led_red;
static struct gpiod_line *led_yellow;
static struct gpiod_line *led_green;

int gpio_lines[NUM_KEYS] = {7,8,9};  // monitor1-3
const char *key_names[NUM_KEYS] = {"MON1","MON2","MON3"};

// Network
static int sockfd;
static struct sockaddr_in dest_addr;
char *monitor_ips[MAX_MONITORS];
int monitor_port;
int current_monitor = 0;

// FreeType
FT_Library ft;
FT_Face face;

// Signal handler
void intHandler(int dummy){
    // ปิด LED ทั้งหมดก่อนออก
    gpiod_line_set_value(led_red, 0);
    gpiod_line_set_value(led_yellow, 0);
    gpiod_line_set_value(led_green, 0);

    gpiod_line_release(do_sw);
    gpiod_line_release(btn_down);
    gpiod_line_release(btn_up);
    gpiod_line_release(btn_done);
    for(int i=0;i<NUM_KEYS;i++) gpiod_line_release(monitor_keys[i]);
    gpiod_chip_close(chip);
    close(sockfd);
    oled_clear();
    oled_display();
    FT_Done_Face(face);
    FT_Done_FreeType(ft);
    printf("\nExiting safely.\n");
    exit(0);
}

// โหลด config จาก env
void load_env_config() {
    monitor_ips[0] = getenv("MONITOR_IP1");
    monitor_ips[1] = getenv("MONITOR_IP2");
    monitor_ips[2] = getenv("MONITOR_IP3");

    if (!monitor_ips[0]) monitor_ips[0] = "192.168.1.244";
    if (!monitor_ips[1]) monitor_ips[1] = "192.168.1.244";
    if (!monitor_ips[2]) monitor_ips[2] = "192.168.1.244";

    char *port_env = getenv("MONITOR_PORT");
    monitor_port = port_env ? atoi(port_env) : 5000;
}

// ตั้ง monitor ปัจจุบัน
void set_monitor(int idx) {
    if(idx < 0 || idx >= MAX_MONITORS) return;

    current_monitor = idx;
    memset(&dest_addr,0,sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(monitor_port);
    if(inet_pton(AF_INET, monitor_ips[current_monitor], &dest_addr.sin_addr)<=0){
        perror("Invalid address");
        exit(1);
    }
    printf("Switched to monitor%d (%s:%d)\n",current_monitor+1,monitor_ips[current_monitor],monitor_port);
}

// debounce
int read_line_debounced(struct gpiod_line *line){
    int last_val = gpiod_line_get_value(line);
    if(last_val < 0) return last_val;
    int stable_count = 0;
    while(stable_count < DEBOUNCE_COUNT){
        usleep(DEBOUNCE_DELAY_US);
        int val = gpiod_line_get_value(line);
        if(val < 0) return val;
        if(val == last_val) stable_count++;
        else { stable_count=0; last_val=val; }
    }
    return last_val;
}

// render ข้อความบน OLED
void render_monitor_text(const char *line1, const char *line2){
    oled_clear();
    render_text(line1,0,30,face);   // บรรทัดบน
    render_text(line2,0,60,face);   // บรรทัดล่าง
    oled_display();
}

// ตรวจสอบ monitor ด้วย ping/pong
int check_monitor(){
    struct sockaddr_in addr = dest_addr;
    char buf[16];
    fd_set readfds;
    struct timeval tv;
    int ret;

    const char *msg = "ping";
    sendto(sockfd,msg,strlen(msg),0,(struct sockaddr*)&addr,sizeof(addr));

    FD_ZERO(&readfds);
    FD_SET(sockfd,&readfds);
    tv.tv_sec = 0;
    tv.tv_usec = PING_TIMEOUT_US;

    ret = select(sockfd+1,&readfds,NULL,NULL,&tv);
    if(ret > 0 && FD_ISSET(sockfd,&readfds)){
        socklen_t len = sizeof(addr);
        int n = recvfrom(sockfd,buf,sizeof(buf)-1,0,(struct sockaddr*)&addr,&len);
        if(n>0){ buf[n]=0; if(strcmp(buf,"pong")==0) return 1; }
    }
    return 0;
}

// แสดง OLED + เช็ค connection
void display_monitor_status(int idx){
    char buf[32];
    snprintf(buf,sizeof(buf),"หน้าจอ: %d",idx+1);
    int connected = check_monitor();
    char buf2[32];
    snprintf(buf2,sizeof(buf2),connected?"เชื่อมต่อ":"            ");
    render_monitor_text(buf,buf2);
}

int main(){
    signal(SIGINT,intHandler);
    load_env_config();

    chip = gpiod_chip_open_by_name("gpiochip0");
    if(!chip){ perror("Open chip failed"); return 1; }


led_red    = gpiod_chip_get_line(chip, 0); // R
led_yellow = gpiod_chip_get_line(chip, 2); // Y
led_green  = gpiod_chip_get_line(chip, 3); // G

if (!led_red || !led_yellow || !led_green) {
    perror("Get LED line failed");
    return 1;
}

if (gpiod_line_request_output(led_red, "led_red", 0) < 0) {
    perror("Request output LED R failed");
    return 1;
}
if (gpiod_line_request_output(led_yellow, "led_yellow", 0) < 0) {
    perror("Request output LED Y failed");
    return 1;
}
if (gpiod_line_request_output(led_green, "led_green", 0) < 0) {
    perror("Request output LED G failed");
    return 1;
}


    // ปุ่ม DO
    do_sw = gpiod_chip_get_line(chip,6);
    if(!do_sw){ perror("Get line failed"); return 1; }
    if(gpiod_line_request_input(do_sw,"sw_do")<0){ perror("Request input failed"); return 1; }

    // ปุ่ม DOWN
    btn_down = gpiod_chip_get_line(chip,21);
    if(!btn_down){ perror("Get line failed"); return 1; }
    if(gpiod_line_request_input(btn_down,"sw_down")<0){ perror("Request input failed"); return 1; }

    // ปุ่ม UP (GPIOA20 / PCM0_DOUT)
    btn_up = gpiod_chip_get_line(chip,20);
    if(!btn_up){ perror("Get line failed"); return 1; }
    if(gpiod_line_request_input(btn_up,"sw_up")<0){ perror("Request input failed"); return 1; }

    // ปุ่ม DONE (SPDIF-OUT / GPIOA17)
    btn_done = gpiod_chip_get_line(chip,17);
    if(!btn_done){ perror("Get line failed"); return 1; }
    if(gpiod_line_request_input(btn_done,"sw_done")<0){ perror("Request input failed"); return 1; }

    // ปุ่ม monitor
    for(int i=0;i<NUM_KEYS;i++){
        monitor_keys[i]=gpiod_chip_get_line(chip,gpio_lines[i]);
        if(!monitor_keys[i]){ perror("Get line failed"); return 1; }
        if(gpiod_line_request_input(monitor_keys[i],"monitor_keys")<0){ perror("Request input failed"); return 1; }
    }

    // socket UDP
    sockfd=socket(AF_INET,SOCK_DGRAM,0);
    if(sockfd<0){ perror("Socket failed"); return 1; }

    set_monitor(current_monitor);

    // OLED + FreeType
    oled_init();
    if (FT_Init_FreeType(&ft)) { fprintf(stderr,"Could not init FreeType\n"); return 1; }
    if (FT_New_Face(ft,FONT_PATH,0,&face)) { fprintf(stderr,"Failed to load font\n"); return 1; }
    FT_Set_Pixel_Sizes(face,0,FONT_SIZE);

    display_monitor_status(current_monitor); // แสดง monitor เริ่มต้น + check

    int last_do_state=1;
    int last_down_state=1;
    int last_up_state=1;
    int last_done_state=1;
    int last_monitor_state[NUM_KEYS]={1,1,1};

    time_t last_check = 0;

    gpiod_line_set_value(led_red, 1);

    while(1){
        // ปุ่ม DO
        int val_do=read_line_debounced(do_sw);
        if(val_do==0 && last_do_state!=0){
            printf("DO pressed! Sending 'do' to monitor %d\n",current_monitor+1);
            const char *msg="do";
            if(sendto(sockfd,msg,strlen(msg),0,(struct sockaddr*)&dest_addr,sizeof(dest_addr))<0)
                perror("Send failed");

            char buf[32]; snprintf(buf,sizeof(buf),"หน้าจอ: %d",current_monitor+1);
            render_monitor_text(buf,"ทำรายการ");
        }
        last_do_state=val_do;

        // ปุ่ม DOWN
        int val_down=read_line_debounced(btn_down);
        if(val_down==0 && last_down_state!=0){
            printf("DOWN pressed! Sending 'down' to monitor %d\n",current_monitor+1);
            const char *msg="down";
            if(sendto(sockfd,msg,strlen(msg),0,(struct sockaddr*)&dest_addr,sizeof(dest_addr))<0)
                perror("Send failed");

            char buf[32]; snprintf(buf,sizeof(buf),"หน้าจอ: %d",current_monitor+1);
            render_monitor_text(buf,"ลง");
        }
        last_down_state=val_down;

        // ปุ่ม UP
        int val_up = read_line_debounced(btn_up);
        if(val_up==0 && last_up_state!=0){
            printf("UP pressed! Sending 'up' to monitor %d\n",current_monitor+1);
            const char *msg="up";
            if(sendto(sockfd,msg,strlen(msg),0,(struct sockaddr*)&dest_addr,sizeof(dest_addr))<0)
                perror("Send failed");

            char buf[32]; snprintf(buf,sizeof(buf),"หน้าจอ: %d",current_monitor+1);
            render_monitor_text(buf,"ขึ้น");
        }
        last_up_state = val_up;

        // ปุ่ม DONE
        int val_done = read_line_debounced(btn_done);
        if(val_done==0 && last_done_state!=0){
            printf("DONE pressed! Sending 'done' to monitor %d\n",current_monitor+1);
            const char *msg="done";
            if(sendto(sockfd,msg,strlen(msg),0,(struct sockaddr*)&dest_addr,sizeof(dest_addr))<0)
                perror("Send failed");

            char buf[32]; snprintf(buf,sizeof(buf),"หน้าจอ: %d",current_monitor+1);
            render_monitor_text(buf,"เสร็จ");
        }
        last_done_state = val_done;

        // ปุ่ม monitor
        for(int i=0;i<NUM_KEYS;i++){
            int val=read_line_debounced(monitor_keys[i]);
            if(val==0 && last_monitor_state[i]!=0){
                set_monitor(i);
                display_monitor_status(i);

                // ส่งข้อความ UDP ไป monitor ที่เลือก
                char msg[4];
                snprintf(msg,sizeof(msg),"m%d",i+1);
                if(sendto(sockfd,msg,strlen(msg),0,(struct sockaddr*)&dest_addr,sizeof(dest_addr))<0)
                    perror("Send failed");

                printf("เลือก monitor%d\n", i+1);

                // แสดงผลบน OLED
                char buf[32]; snprintf(buf,sizeof(buf),"หน้าจอ: %d",i+1);
                render_monitor_text(buf,"เลือกจอ");

        // ✅ ควบคุม LED ตาม monitor
        gpiod_line_set_value(led_red,   i==0 ? 1 : 0);   // monitor1 -> R
        gpiod_line_set_value(led_yellow,i==1 ? 1 : 0);   // monitor2 -> Y
        gpiod_line_set_value(led_green, i==2 ? 1 : 0);   // monitor3 -> G

            }
            last_monitor_state[i]=val;
        }

        // ตรวจสอบ connection ทุก 5 วินาที
        if(difftime(time(NULL), last_check) >= 5){
            oled_clear_line(56, 16); // เคลียร์แถวล่าง

            char buf[32];
            snprintf(buf,sizeof(buf),"หน้าจอ: %d",current_monitor+1);
            int connected = check_monitor();
            char buf2[32];
            snprintf(buf2,sizeof(buf2),connected?"เชื่อมต่อ":"            ");
            render_monitor_text(buf,buf2);

            last_check = time(NULL);
        }

        usleep(20000);
    }

    // Clean FreeType
    FT_Done_Face(face);
    FT_Done_FreeType(ft);

    return 0;
}
