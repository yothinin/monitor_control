# Monitor Control for NanoPi M1

โปรเจกต์นี้เป็นระบบควบคุมหน้าจอ OLED ขนาด 128x64 บน **NanoPi M1** สำหรับแสดงสถานะ monitor และสั่งงานผ่านปุ่ม GPIO + UDP

---

## ฟีเจอร์หลัก

- แสดงหน้าจอปัจจุบัน (Monitor 1-3)
- แสดงข้อความ **เชื่อมต่อ** หรือว่าง ตามผลตอบกลับ `pong` จากเครื่อง monitor
- ปุ่ม **do** → ส่ง UDP "do" ไป monitor + แสดงข้อความ `btn_do pressed` ชั่วคราว
- ปุ่ม monitor 1-3 → เปลี่ยนหน้าจอ + แสดงสถานะ + ตรวจสอบการเชื่อมต่อทุก 5 วินาที
- ใช้ **FreeType** สำหรับแสดงข้อความภาษาไทยบน OLED

---

## ไฟล์ในโปรเจกต์

monitor_control/
├─ monitor_control.c
├─ oled_i2c.h
├─ oled_i2c.c
└─ fonts/
└─ NotoSerifThai.ttf


---

## การติดตั้ง Environment

ติดตั้ง dependencies บน Debian/Ubuntu/NanoPi M1:

```bash
sudo apt update
sudo apt install build-essential libgpiod-dev libfreetype6-dev git

เพิ่มฟอนต์ภาษาไทย:

mkdir fonts
cp /path/to/NotoSerifThai.ttf fonts/

การคอมไพล์
gcc monitor_control.c oled_i2c.c -o monitor_control \
    -I/usr/include/freetype2 -lgpiod -lfreetype -lpthread


-I/usr/include/freetype2 → include path ของ FreeType

-lgpiod → GPIO library

-lfreetype → FreeType library

-lpthread → สำหรับ UDP listener thread

การใช้งาน

รันโปรแกรมด้วยสิทธิ์ root:

sudo ./monitor_control

