#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h> // NVS storage for the on-device WiFi manager
#include <SPI.h>
#include <SD.h> // TF/micro-SD card slot - see sdLoadWifiNetworks()/sdSaveWifiNetwork()
#include "esp_system.h" // esp_reset_reason() - TEMP/DEBUG, see setup()

#include <lvgl.h>
#include <TFT_eSPI.h>

#include "secrets.h"
//********************************************************************************************//
/*******************************************************************************
   End of Arduino_GFX setting
 ******************************************************************************/
//I2C读写命令
#define GT_CMD_WR           0XBA         //写命令0xBA
#define GT_CMD_RD           0XBB         //读命令0XBB
//
//#define GT_CMD_WR           0X28         //
//#define GT_CMD_RD           0X29         //

#define GT911_MAX_WIDTH     320          //Touchscreen pad max width
#define GT911_MAX_HEIGHT    480          //Touchscreen pad max height

//GT911 部分寄存器定义
#define GT_CTRL_REG         0X8040       //GT911控制寄存器
#define GT_CFGS_REG         0X8047       //GT911配置起始地址寄存器
#define GT_CHECK_REG        0X80FF       //GT911校验和寄存器
#define GT_PID_REG          0X8140       //GT911产品ID寄存器

#define GT_GSTID_REG        0X814E       //GT911当前检测到的触摸情况
#define GT911_READ_XY_REG   0x814E       /* 坐标寄存器 */
#define CT_MAX_TOUCH        5            //电容触摸屏最大支持的点数

int IIC_SCL = 32;
int IIC_SDA = 33;
int IIC_RST = 25;
//int IIC_INT = 21;//36

#define IIC_SCL_0  digitalWrite(IIC_SCL,LOW)
#define IIC_SCL_1  digitalWrite(IIC_SCL,HIGH)

#define IIC_SDA_0  digitalWrite(IIC_SDA,LOW)
#define IIC_SDA_1  digitalWrite(IIC_SDA,HIGH)

#define IIC_RST_0  digitalWrite(IIC_RST,LOW)
#define IIC_RST_1  digitalWrite(IIC_RST,HIGH)

#define READ_SDA   digitalRead(IIC_SDA)

typedef struct
{
  uint8_t Touch;
  uint8_t TouchpointFlag;
  uint8_t TouchCount;

  uint8_t Touchkeytrackid[CT_MAX_TOUCH];
  uint16_t X[CT_MAX_TOUCH];
  uint16_t Y[CT_MAX_TOUCH];
  uint16_t S[CT_MAX_TOUCH];
} GT911_Dev;
GT911_Dev Dev_Now, Dev_Backup;
bool touched = 0;     //没有使用触摸中断，有触摸标志位touched = 1，否则touched = 0
uint8_t s_GT911_CfgParams[] =
{
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};


void delay_us(unsigned int xus)  //1us
{
  for (; xus > 1; xus--);
}
void SDA_IN(void)
{
  pinMode(IIC_SDA, INPUT);

}

void SDA_OUT(void)
{
  pinMode(IIC_SDA, OUTPUT);
}

//初始化IIC
void IIC_Init(void)
{
  pinMode(IIC_SDA, OUTPUT);
  pinMode(IIC_SCL, OUTPUT);
  pinMode(IIC_RST, OUTPUT);
  //pinMode(IIC_INT, INPUT);
//  pinMode(IIC_INT, OUTPUT);
  //  attachInterrupt(IIC_INT, isr, FALLING);
  IIC_SCL_1;
  IIC_SDA_1;

}
//产生IIC起始信号
void IIC_Start(void)
{
  SDA_OUT();
  IIC_SDA_1;
  IIC_SCL_1;
  delay_us(4);
  IIC_SDA_0; //START:when CLK is high,DATA change form high to low
  delay_us(4);
  IIC_SCL_0; //钳住I2C总线，准备发送或接收数据
}
//产生IIC停止信号
void IIC_Stop(void)
{
  SDA_OUT();
  IIC_SCL_0;
  IIC_SDA_0; //STOP:when CLK is high DATA change form low to high
  delay_us(4);
  IIC_SCL_1;
  IIC_SDA_1; //发送I2C总线结束信号
  delay_us(4);
}
//等待应答信号到来
//返回值：1，接收应答失败
//        0，接收应答成功
uint8_t IIC_Wait_Ack(void)
{
  uint8_t ucErrTime = 0;
  SDA_IN();      //SDA设置为输入
  IIC_SDA_1; delay_us(1);
  IIC_SCL_1; delay_us(1);
  while (READ_SDA)
  {
    ucErrTime++;
    if (ucErrTime > 250)
    {
      IIC_Stop();
      return 1;
    }
  }
  IIC_SCL_0; //时钟输出0
  return 0;
}
//产生ACK应答
void IIC_Ack(void)
{
  IIC_SCL_0;
  SDA_OUT();
  IIC_SDA_0;
  delay_us(2);
  IIC_SCL_1;
  delay_us(2);
  IIC_SCL_0;
}
//不产生ACK应答
void IIC_NAck(void)
{
  IIC_SCL_0;
  SDA_OUT();
  IIC_SDA_1;
  delay_us(2);
  IIC_SCL_1;
  delay_us(2);
  IIC_SCL_0;
}
//IIC发送一个字节
//返回从机有无应答
//1，有应答
//0，无应答
void IIC_Send_Byte(uint8_t txd)
{
  uint8_t t;
  SDA_OUT();
  IIC_SCL_0; //拉低时钟开始数据传输
  for (t = 0; t < 8; t++)
  {
    //IIC_SDA=(txd&0x80)>>7;
    if ((txd & 0x80) >> 7)
      IIC_SDA_1;
    else
      IIC_SDA_0;
    txd <<= 1;
    delay_us(2);   //对TEA5767这三个延时都是必须的
    IIC_SCL_1;
    delay_us(2);
    IIC_SCL_0;
    delay_us(2);
  }
}
//读1个字节，ack=1时，发送ACK，ack=0，发送nACK
uint8_t IIC_Read_Byte(unsigned char ack)
{
  unsigned char i, receive = 0;
  SDA_IN();//SDA设置为输入
  for (i = 0; i < 8; i++ )
  {
    IIC_SCL_0;
    delay_us(2);
    IIC_SCL_1;
    receive <<= 1;
    if (READ_SDA)receive++;
    delay_us(1);
  }
  if (!ack)
    IIC_NAck();//发送nACK
  else
    IIC_Ack(); //发送ACK
  return receive;
}


// Bit-banged I2C timing (delay_us() below is a plain busy-wait loop, not an
// RTOS-aware delay) can get corrupted if a higher-priority task - notably
// WiFi's, which runs aggressively during WiFi.scanNetworks() - preempts the
// CPU mid-transaction. Touch going completely unresponsive after a WiFi
// scan (confirmed on real hardware: zero raw touch reads afterward, not
// just missed clicks) is consistent with that. Wrapping each full
// transaction in a critical section (interrupts/task-switching disabled on
// this core) protects it; transactions here are short (a handful of bytes
// for normal touch polling, up to ~186 for the one-time GT911 config dump
// in gt911_int_()), so the section stays brief.
static portMUX_TYPE gt911_i2c_mux = portMUX_INITIALIZER_UNLOCKED;

//reg:起始寄存器地址
//buf:数据缓缓存区
//len:写数据长度
//返回值:0,成功;1,失败.
uint8_t GT911_WR_Reg(uint16_t reg, uint8_t *buf, uint8_t len)
{
  portENTER_CRITICAL(&gt911_i2c_mux);
  uint8_t i;
  uint8_t ret = 0;
  IIC_Start();
  IIC_Send_Byte(GT_CMD_WR);       //发送写命令
  IIC_Wait_Ack();
  IIC_Send_Byte(reg >> 8);     //发送高8位地址
  IIC_Wait_Ack();
  IIC_Send_Byte(reg & 0XFF);     //发送低8位地址
  IIC_Wait_Ack();
  for (i = 0; i < len; i++)
  {
    IIC_Send_Byte(buf[i]);      //发数据
    ret = IIC_Wait_Ack();
    if (ret)break;
  }
  IIC_Stop();                    //产生一个停止条件
  portEXIT_CRITICAL(&gt911_i2c_mux);
  return ret;
}

//reg:起始寄存器地址
//buf:数据缓缓存区
//len:读数据长度
void GT911_RD_Reg(uint16_t reg, uint8_t *buf, uint8_t len)
{
  portENTER_CRITICAL(&gt911_i2c_mux);
  uint8_t i;
  IIC_Start();
  IIC_Send_Byte(GT_CMD_WR);   //发送写命令
  IIC_Wait_Ack();
  IIC_Send_Byte(reg >> 8);     //发送高8位地址
  IIC_Wait_Ack();
  IIC_Send_Byte(reg & 0XFF);     //发送低8位地址
  IIC_Wait_Ack();
  IIC_Start();
  IIC_Send_Byte(GT_CMD_RD);   //发送读命令
  IIC_Wait_Ack();
  for (i = 0; i < len; i++)
  {
    buf[i] = IIC_Read_Byte(i == (len - 1) ? 0 : 1); //发数据
  }
  IIC_Stop();//产生一个停止条件
  portEXIT_CRITICAL(&gt911_i2c_mux);
}

//发送配置参数
//mode:0,参数不保存到flash
//     1,参数保存到flash
uint8_t GT911_Send_Cfg(uint8_t mode)
{
  uint8_t buf[2];
  uint8_t i = 0;
  buf[0] = 0;
  buf[1] = mode;  //是否写入到GT911 FLASH?  即是否掉电保存
  //     for(i=0;i<sizeof(GT911_Cfg);i++)buf[0]+=GT911_Cfg[i];//计算校验和
  //     buf[0]=(~buf[0])+1;
  //GT911_WR_Reg(GT_CFGS_REG,(uint8_t*)GT911_Cfg,sizeof(GT911_Cfg));//发送寄存器配置
  GT911_WR_Reg(GT_CHECK_REG, buf, 2); //写入校验和,和配置更新标记
  return 0;
}

void GT911_Scan(void)
{
  uint8_t buf[41];
  uint8_t Clearbuf = 0;
  uint8_t i;
  if (1)
    // if (Dev_Now.Touch == 1)
  {
    Dev_Now.Touch = 0;
    GT911_RD_Reg(GT911_READ_XY_REG, buf, 1);

    if ((buf[0] & 0x80) == 0x00)
    {
      touched = 0;
      GT911_WR_Reg(GT911_READ_XY_REG, (uint8_t *)&Clearbuf, 1);
      // Serial.printf("No touch\r\n"); // silenced: this fires ~100x/sec and
      // drowns out the touch-calibration diagnostic prints below.
      // (delay(10) that used to sit here on every untouched poll was pure
      // latency for no benefit - removed; it was making rapid T9 multi-taps
      // feel sluggish since this runs on every single touch poll.)
    }
    else
    {
      touched = 1;
      Dev_Now.TouchpointFlag = buf[0];
      Dev_Now.TouchCount = buf[0] & 0x0f;
      if (Dev_Now.TouchCount > 5)
      {
        touched = 0;
        GT911_WR_Reg(GT911_READ_XY_REG, (uint8_t *)&Clearbuf, 1);
        Serial.printf("Dev_Now.TouchCount > 5\r\n");
        return ;
      }
      GT911_RD_Reg(GT911_READ_XY_REG + 1, &buf[1], Dev_Now.TouchCount * 8);
      GT911_WR_Reg(GT911_READ_XY_REG, (uint8_t *)&Clearbuf, 1);

      Dev_Now.Touchkeytrackid[0] = buf[1];
      Dev_Now.X[0] = ((uint16_t)buf[3] << 8) + buf[2];
      Dev_Now.Y[0] = ((uint16_t)buf[5] << 8) + buf[4];
      Dev_Now.S[0] = ((uint16_t)buf[7] << 8) + buf[6];

      Dev_Now.Touchkeytrackid[1] = buf[9];
      Dev_Now.X[1] = ((uint16_t)buf[11] << 8) + buf[10];
      Dev_Now.Y[1] = ((uint16_t)buf[13] << 8) + buf[12];
      Dev_Now.S[1] = ((uint16_t)buf[15] << 8) + buf[14];

      Dev_Now.Touchkeytrackid[2] = buf[17];
      Dev_Now.X[2] = ((uint16_t)buf[19] << 8) + buf[18];
      Dev_Now.Y[2] = ((uint16_t)buf[21] << 8) + buf[20];
      Dev_Now.S[2] = ((uint16_t)buf[23] << 8) + buf[22];

      Dev_Now.Touchkeytrackid[3] = buf[25];
      Dev_Now.X[3] = ((uint16_t)buf[27] << 8) + buf[26];
      Dev_Now.Y[3] = ((uint16_t)buf[29] << 8) + buf[28];
      Dev_Now.S[3] = ((uint16_t)buf[31] << 8) + buf[30];

      Dev_Now.Touchkeytrackid[4] = buf[33];
      Dev_Now.X[4] = ((uint16_t)buf[35] << 8) + buf[34];
      Dev_Now.Y[4] = ((uint16_t)buf[37] << 8) + buf[36];
      Dev_Now.S[4] = ((uint16_t)buf[39] << 8) + buf[38];
//     Serial.printf("X[0]:%d,Y[0]:%d\r\n",  Dev_Now.X[0], Dev_Now.Y[0]);
      for (i = 0; i < Dev_Backup.TouchCount; i++)
      {
        //if (Dev_Now.Y[i] < 22)Dev_Now.Y[i] = 22;
        //if (Dev_Now.Y[i] > 460)Dev_Now.Y[i] = 460;
        //if (Dev_Now.X[i] < 20)Dev_Now.X[i] = 20;
        //if (Dev_Now.X[i] > 779)Dev_Now.X[i] = 779;

        
        if (Dev_Now.Y[i] < 0)Dev_Now.Y[i] = 0;
        if (Dev_Now.Y[i] > 480)Dev_Now.Y[i] = 480;
        if (Dev_Now.X[i] < 0)Dev_Now.X[i] = 0;
        if (Dev_Now.X[i] > 320)Dev_Now.X[i] = 320;

        //Serial.printf("Dev_Backup.X[%d]:%d,Dev_Backup.Y[%d]:%d\r\n", i, Dev_Backup.X[i],i, Dev_Backup.Y[i]);
      }
      for (i = 0; i < Dev_Now.TouchCount; i++)
    {
        //if (Dev_Now.Y[i] < 22)Dev_Now.Y[i] = 22;
        //if (Dev_Now.Y[i] > 460)Dev_Now.Y[i] = 460;
        //if (Dev_Now.X[i] < 20)Dev_Now.X[i] = 20;
        //if (Dev_Now.X[i] > 779)Dev_Now.X[i] = 779;

        if (Dev_Now.Y[i] < 0)touched = 0;
        if (Dev_Now.Y[i] > 480)touched = 0;
        if (Dev_Now.X[i] < 0)touched = 0;
        if (Dev_Now.X[i] > 320)touched = 0;

        if(touched == 1)
        {
            Dev_Backup.X[i] = Dev_Now.X[i];
            Dev_Backup.Y[i] = Dev_Now.Y[i];
            Dev_Backup.TouchCount = Dev_Now.TouchCount;

            //Serial.printf("Dev_NowX[%d]:%d,Dev_NowY[%d]:%d\r\n", i, Dev_Now.X[i],i,  Dev_Now.Y[i]);
        }
      }
     if(Dev_Now.TouchCount==0)
        {
            touched = 0;
        }  
    }
  }
}

uint8_t GT911_ReadStatue(void)
{
  uint8_t buf[4];
  GT911_RD_Reg(GT_PID_REG, (uint8_t *)&buf[0], 3); 
  GT911_RD_Reg(GT_CFGS_REG, (uint8_t *)&buf[3], 1);
  Serial.printf("TouchPad_ID:%d,%d,%d\r\nTouchPad_Config_Version:%2x\r\n", buf[0], buf[1], buf[2], buf[3]);
  return buf[3];
}

void Interrupt_callBack() {
  Serial.printf("ARDUINO_ISR_ATTR:\r\n");
}
void GT911_Reset_Sequence()
{
  //此处RST引脚与屏幕RST共用，只需要初始化一次即可
  IIC_RST_0;
  delay(100);
  IIC_RST_0;
  delay(100);
  IIC_RST_1;
  delay(200);

  //INT_Config();
  //  delay(100);
}

void GT911_Int()
{
  uint8_t config_Checksum = 0, i;

  IIC_Init();
  GT911_Reset_Sequence();
  //debug
  GT911_RD_Reg(GT_CFGS_REG, (uint8_t *)&s_GT911_CfgParams[0], 186);

  for (i = 0; i < sizeof(s_GT911_CfgParams) - 2; i++)
  {
    config_Checksum += s_GT911_CfgParams[i];

    Serial.printf("0x%02X  ", s_GT911_CfgParams[i]);
    if ((i + 1) % 10 == 0)
      Serial.printf("\r\n");
  }
  Serial.printf("0x%02X  0x%02X\r\nconfig_Checksum=0x%2X\r\n", s_GT911_CfgParams[184], s_GT911_CfgParams[185], ((~config_Checksum) + 1) & 0xff);

  if (s_GT911_CfgParams[184] == (((~config_Checksum) + 1) & 0xff))
  {
    Serial.printf("READ CONFIG SUCCESS!\r\n");
    Serial.printf("%d*%d\r\n", s_GT911_CfgParams[2] << 8 | s_GT911_CfgParams[1], s_GT911_CfgParams[4] << 8 | s_GT911_CfgParams[3]);

    if ((GT911_MAX_WIDTH != (s_GT911_CfgParams[2] << 8 | s_GT911_CfgParams[1])) || (GT911_MAX_HEIGHT != (s_GT911_CfgParams[4] << 8 | s_GT911_CfgParams[3])))
    {
      s_GT911_CfgParams[1] = GT911_MAX_WIDTH & 0xff;
      s_GT911_CfgParams[2] = GT911_MAX_WIDTH >> 8;
      s_GT911_CfgParams[3] = GT911_MAX_HEIGHT & 0xff;
      s_GT911_CfgParams[4] = GT911_MAX_HEIGHT >> 8;
      s_GT911_CfgParams[185] = 1;

      Serial.printf("%d*%d\r\n", s_GT911_CfgParams[2] << 8 | s_GT911_CfgParams[1], s_GT911_CfgParams[4] << 8 | s_GT911_CfgParams[3]);

      config_Checksum = 0;
      for (i = 0; i < sizeof(s_GT911_CfgParams) - 2; i++)
      {
        config_Checksum += s_GT911_CfgParams[i];
      }
      s_GT911_CfgParams[184] = (~config_Checksum) + 1;

      Serial.printf("config_Checksum=0x%2X\r\n", s_GT911_CfgParams[184]);

      Serial.printf("\r\n*************************\r\n");
      for (i = 0; i < sizeof(s_GT911_CfgParams); i++)
      {
        Serial.printf("0x%02X  ", s_GT911_CfgParams[i]);
        if ((i + 1) % 10 == 0)
          Serial.printf("\r\n");
      }
      Serial.printf("\r\n*************************\r\n");
      GT911_WR_Reg(GT_CFGS_REG, (uint8_t *)s_GT911_CfgParams, sizeof(s_GT911_CfgParams));


      GT911_RD_Reg(GT_CFGS_REG, (uint8_t *)&s_GT911_CfgParams[0], 186);

      config_Checksum = 0;
      for (i = 0; i < sizeof(s_GT911_CfgParams) - 2; i++)
      {
        config_Checksum += s_GT911_CfgParams[i];

        Serial.printf("0x%02X  ", s_GT911_CfgParams[i]);
        if ((i + 1) % 10 == 0)
          Serial.printf("\r\n");
      }
      Serial.printf("0x%02X  ", s_GT911_CfgParams[184]);
      Serial.printf("0x%02X  ", s_GT911_CfgParams[185]);
      Serial.printf("\r\n");
      Serial.printf("config_Checksum=0x%2X\r\n", ((~config_Checksum) + 1) & 0xff);
    }

  }
  GT911_ReadStatue();
}



/* Display flushing */


/*Read the touchpad*/

// GT911 reports raw touch coordinates in the panel's native PORTRAIT
// orientation, but the display runs in LANDSCAPE (tft.setRotation(1),
// 480x320 - see setup()), so raw X/Y must be swapped and rescaled to match
// what's drawn on screen: screenX = f(rawY), screenY = f(rawX) (90 deg
// rotation, confirmed on-device - no extra axis flip needed).
//
// The scale/offset for that mapping is NOT a fixed constant - it's derived
// at boot by runTouchCalibration() (see below), which shows two on-screen
// targets and captures the actual raw reading at each tap. That replaces
// guessing the raw range from manually-reported corner taps, which was
// fragile (small panel, hard to reach the literal physical edge, and a
// linear stretch amplifies any imprecision in those reference points).
struct TouchCalib {
  float scaleX = 1, offsetX = 0; // screenX = rawY * scaleX + offsetX
  float scaleY = 1, offsetY = 0; // screenY = rawX * scaleY + offsetY
};
TouchCalib g_touchCalib;

// Raw GT911 reading, no screen-coordinate transform - used both by the
// calibration routine and (after transform) by my_touchpad_read() below.
bool readRawTouch(int32_t &rawX, int32_t &rawY) {
  GT911_Scan();
  if (!touched) return false;
  rawX = Dev_Now.X[0];
  rawY = Dev_Now.Y[0];
  Serial.printf("RAW TOUCH detected: x:%d y:%d\r\n", rawX, rawY); // TEMP/DEBUG
  return true;
}

  void my_touchpad_read( lv_indev_drv_t * indev_driver, lv_indev_data_t * data )
  {
  int32_t rawX, rawY;
  if ( !readRawTouch(rawX, rawY) )
  {
    data->state = LV_INDEV_STATE_REL;
  }
  else
  {
    int32_t screenX = (int32_t)(rawY * g_touchCalib.scaleX + g_touchCalib.offsetX);
    int32_t screenY = (int32_t)(rawX * g_touchCalib.scaleY + g_touchCalib.offsetY);

    // Clamp in case a touch falls outside the calibrated range
    if (screenX < 0) screenX = 0;
    if (screenX > 479) screenX = 479;
    if (screenY < 0) screenY = 0;
    if (screenY > 319) screenY = 319;

    data->point.x = screenX;
    data->point.y = screenY;
    data->state = LV_INDEV_STATE_PR;

    static uint32_t lastLog = 0; // TEMP/DEBUG: throttle to ~10/sec, still readable
    if (millis() - lastLog > 100) {
      Serial.printf("TAP screen x:%d y:%d (raw x:%d y:%d)\r\n", screenX, screenY, rawX, rawY);
      lastLog = millis();
    }
  }
}

// Waits for any already-in-progress touch to release, then waits for and
// returns the raw coordinates of the next fresh tap. Keeps LVGL painting
// (lv_timer_handler) so the calibration target stays visible while blocked.
void waitForRawTap(int32_t &rawX, int32_t &rawY) {
  int32_t x, y;
  // NOTE: deliberately does NOT call lv_timer_handler() here - that would
  // also trigger the indev's own GT911_Scan() via my_touchpad_read() in the
  // background, racing with these direct reads (GT911_Scan() clears the
  // "data ready" flag on every read, so two independent pollers can each
  // clear the flag out from under the other and miss the tap entirely).
  // The target is already drawn before this is called, so no redraw is
  // needed while waiting.
  while (readRawTouch(x, y)) { delay(5); }

  uint32_t lastBeat = millis(); // TEMP/DEBUG heartbeat, proves we're not hung
  while (!readRawTouch(rawX, rawY)) {
    delay(5);
    if (millis() - lastBeat > 1000) {
      Serial.println("...waiting for tap...");
      lastBeat = millis();
    }
  }
  delay(30); // let the reading settle
  readRawTouch(rawX, rawY);
}

// Shows two on-screen targets, captures the real raw touch at each, and
// derives g_touchCalib from those two points. Run once at boot before the
// chat UI is built.
void runTouchCalibration() {
  lv_obj_clean(lv_scr_act());
  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x0f172a), 0);

  lv_obj_t *label = lv_label_create(lv_scr_act());
  lv_label_set_text(label, "Tap the red dot");
  lv_obj_set_style_text_color(label, lv_color_hex(0xffffff), 0);
  lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 10);

  lv_obj_t *target = lv_obj_create(lv_scr_act());
  lv_obj_set_size(target, 20, 20);
  lv_obj_set_style_radius(target, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(target, lv_color_hex(0xff0000), 0);
  lv_obj_set_style_border_width(target, 0, 0);
  lv_obj_clear_flag(target, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(target, LV_OBJ_FLAG_CLICKABLE);

  // Pulled well clear of the true edges - on-device testing found a dead
  // strip near the top (and likely the right) of the touch-sensitive area
  // *in this orientation* (tft.setRotation(1) - see setup()). These points
  // were tuned specifically against that dead zone; a setRotation(3)
  // experiment moved the dead zone and broke them (both too-narrow and
  // too-wide variants were tried and failed), so if the display orientation
  // ever changes again, re-map the dead zone via the corner-tap diagnostic
  // in my_touchpad_read()'s TAP/RAW TOUCH detected Serial prints rather than
  // guessing new coordinates.
  const int32_t P1X = 70,  P1Y = 110;
  const int32_t P2X = 380, P2Y = 260;
  // If both taps land within this many raw units of each other, treat the
  // attempt as a mis-tap (e.g. user tapped point 2 before noticing it moved,
  // reusing point 1's position) and retry rather than divide-by-near-zero
  // into an inf/-inf scale that clamps every touch to one screen corner.
  const int32_t MIN_RAW_SEPARATION = 20;

  int32_t r1x, r1y, r2x, r2y;
  for (;;) {
    lv_label_set_text(label, "Tap the red dot (1/2)");
    lv_obj_set_style_bg_color(target, lv_color_hex(0xff0000), 0);
    lv_obj_set_pos(target, P1X - 10, P1Y - 10);
    lv_obj_clear_flag(target, LV_OBJ_FLAG_HIDDEN);
    lv_refr_now(NULL);
    waitForRawTap(r1x, r1y);

    // Force the user to actually see the dot move before tapping again -
    // also gives a still-touching finger time to lift.
    lv_label_set_text(label, "Good! Now tap the dot (2/2)");
    lv_obj_set_style_bg_color(target, lv_color_hex(0x22c55e), 0);
    lv_obj_set_pos(target, P2X - 10, P2Y - 10);
    lv_refr_now(NULL);
    delay(600);
    waitForRawTap(r2x, r2y);

    if (abs(r2x - r1x) >= MIN_RAW_SEPARATION && abs(r2y - r1y) >= MIN_RAW_SEPARATION) {
      break; // good calibration data, proceed
    }
    Serial.printf("Calibration taps too close (r1=%d,%d r2=%d,%d) - retrying\r\n",
                  r1x, r1y, r2x, r2y);
    lv_label_set_text(label, "Too close together - try again");
    lv_obj_set_style_bg_color(target, lv_color_hex(0xff0000), 0);
    lv_refr_now(NULL);
    delay(1200);
  }

  g_touchCalib.scaleX = (float)(P2X - P1X) / (float)(r2y - r1y);
  g_touchCalib.offsetX = P1X - r1y * g_touchCalib.scaleX;
  g_touchCalib.scaleY = (float)(P2Y - P1Y) / (float)(r2x - r1x);
  g_touchCalib.offsetY = P1Y - r1x * g_touchCalib.scaleY;

  Serial.printf("Touch calibrated: scaleX=%.4f offsetX=%.2f scaleY=%.4f offsetY=%.2f\r\n",
                g_touchCalib.scaleX, g_touchCalib.offsetX,
                g_touchCalib.scaleY, g_touchCalib.offsetY);

  lv_label_set_text(label, "Calibrated!");
  lv_obj_add_flag(target, LV_OBJ_FLAG_HIDDEN);
  lv_refr_now(NULL);
  delay(400);
}
//*****************************************************************************************************//

/*更改屏幕分辨率 - Landscape mode*/
static const uint16_t screenWidth  = 480;
static const uint16_t screenHeight = 320;

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[screenWidth * 10];

TFT_eSPI tft = TFT_eSPI(); /* TFT实例 */

// WiFi / weather / Gemini credentials live in secrets.h (gitignored) — see
// secrets.example.h for the template.

// NTP Configuration
const char* NTP_SERVER = "pool.ntp.org";
const long GMT_OFFSET = 7 * 3600; // GMT+7 (Indonesia)
const int DAYLIGHT_OFFSET = 0;

// Touch debounce variables
unsigned long lastTouchTime = 0;
const unsigned long TOUCH_DEBOUNCE_MS = 3000; // 3 seconds minimum between touches

#if LV_USE_LOG != 0
/* 串行调试 */
void my_print(const char * buf)
{
  Serial.printf(buf);
  Serial.flush();
}
#endif
//_______________________
void lv_example_btn(void)
{
  /*要转换的属性*/
  static lv_style_prop_t props[] = {
    LV_STYLE_TRANSFORM_WIDTH, LV_STYLE_TRANSFORM_HEIGHT, LV_STYLE_TEXT_LETTER_SPACE
  };

  /*Transition descriptor when going back to the default state.
    Add some delay to be sure the press transition is visible even if the press was very short*/
  static lv_style_transition_dsc_t transition_dsc_def;
  lv_style_transition_dsc_init(&transition_dsc_def, props, lv_anim_path_overshoot, 250, 100, NULL);

  /*Transition descriptor when going to pressed state.
    No delay, go to presses state immediately*/
  static lv_style_transition_dsc_t transition_dsc_pr;
  lv_style_transition_dsc_init(&transition_dsc_pr, props, lv_anim_path_ease_in_out, 250, 0, NULL);

  /*Add only the new transition to he default state*/
  static lv_style_t style_def;
  lv_style_init(&style_def);
  lv_style_set_transition(&style_def, &transition_dsc_def);

  /*Add the transition and some transformation to the presses state.*/
  static lv_style_t style_pr;
  lv_style_init(&style_pr);
  lv_style_set_transform_width(&style_pr, 10);
  lv_style_set_transform_height(&style_pr, -10);
  lv_style_set_text_letter_space(&style_pr, 10);
  lv_style_set_transition(&style_pr, &transition_dsc_pr);

  lv_obj_t * btn1 = lv_btn_create(lv_scr_act());
  lv_obj_align(btn1, LV_ALIGN_CENTER, 0, -80);
  lv_obj_add_style(btn1, &style_pr, LV_STATE_PRESSED);
  lv_obj_add_style(btn1, &style_def, 0);

  lv_obj_t * label = lv_label_create(btn1);
  lv_label_set_text(label, "btn1");

  /*Init the style for the default state*/
  static lv_style_t style;
  lv_style_init(&style);

  lv_style_set_radius(&style, 3);

  lv_style_set_bg_opa(&style, LV_OPA_100);
  lv_style_set_bg_color(&style, lv_palette_main(LV_PALETTE_BLUE));
  lv_style_set_bg_grad_color(&style, lv_palette_darken(LV_PALETTE_BLUE, 2));
  lv_style_set_bg_grad_dir(&style, LV_GRAD_DIR_VER);

  lv_style_set_border_opa(&style, LV_OPA_40);
  lv_style_set_border_width(&style, 2);
  lv_style_set_border_color(&style, lv_palette_main(LV_PALETTE_GREY));

  lv_style_set_shadow_width(&style, 8);
  lv_style_set_shadow_color(&style, lv_palette_main(LV_PALETTE_GREY));
  lv_style_set_shadow_ofs_y(&style, 8);

  lv_style_set_outline_opa(&style, LV_OPA_COVER);
  lv_style_set_outline_color(&style, lv_palette_main(LV_PALETTE_BLUE));

  lv_style_set_text_color(&style, lv_color_white());
  lv_style_set_pad_all(&style, 10);

  /*Init the pressed style*/
  static lv_style_t style_pr_2;
  lv_style_init(&style_pr_2);

  /*Ad a large outline when pressed*/
  lv_style_set_outline_width(&style_pr_2, 30);
  lv_style_set_outline_opa(&style_pr_2, LV_OPA_TRANSP);

  lv_style_set_translate_y(&style_pr_2, 5);
  lv_style_set_shadow_ofs_y(&style_pr_2, 3);
  lv_style_set_bg_color(&style_pr_2, lv_palette_darken(LV_PALETTE_BLUE, 2));
  lv_style_set_bg_grad_color(&style_pr_2, lv_palette_darken(LV_PALETTE_BLUE, 4));

  /*Add a transition to the the outline*/
  static lv_style_transition_dsc_t trans;
  static lv_style_prop_t props2[] = {LV_STYLE_OUTLINE_WIDTH, LV_STYLE_OUTLINE_OPA};
  lv_style_transition_dsc_init(&trans, props2, lv_anim_path_linear, 300, 0, NULL);

  lv_style_set_transition(&style_pr_2, &trans);

  lv_obj_t * btn2 = lv_btn_create(lv_scr_act());
  lv_obj_remove_style_all(btn2);                          /*Remove the style coming from the theme*/
  lv_obj_add_style(btn2, &style, 0);
  lv_obj_add_style(btn2, &style_pr_2, LV_STATE_PRESSED);
  lv_obj_set_size(btn2, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_center(btn2);

  lv_obj_t * label2 = lv_label_create(btn2);
  lv_label_set_text(label2, "Button");
  lv_obj_center(label2);
}
//_______________________

// (Old hardcoded-credentials connectToWiFi() removed - replaced by the
// NVS-backed on-device WiFi manager further down: loadSavedWifi(),
// tryConnectWifi(), runWifiSetupFlow(), wired up in setup().)
bool tryConnectWifi(const String &ssid, const String &pass, lv_obj_t *statusLabel); // fwd decl - connectToAnySavedWifi() (defined above it) needs this

// (Weather dashboard/fetch code removed - this build is chat-only: keyboard +
// input box + Gemini reply. See createChatScreen() below.)

// ---------------------------------------------------------------------------
// AI Companion chat screen (Gemini text chat) - this is now the ONLY screen.
// ---------------------------------------------------------------------------
// A reply area, a one-line textarea, a Send button, and an lv_keyboard that
// pops up when the textarea is focused. Touch input already flows through
// GT911 -> my_touchpad_read -> lvgl indev, so the keyboard works for free.
//
// NOTE: queryGemini() blocks on HTTPClient for the duration of the request
// (roughly 1-3s), which stalls lv_timer_handler()/animations for that time.
// Fine for this MVP; if that stall becomes noticeable, move the call into
// its own FreeRTOS task and hand the result back through a queue/flag
// (see the Atomic<> critical-section pattern in include/ESP323248S035.hpp).

lv_obj_t * chat_input_ta = nullptr;
lv_obj_t * chat_response_label = nullptr;
lv_obj_t * chat_kb = nullptr;
lv_obj_t * chat_response_box = nullptr;
lv_obj_t * chat_send_btn = nullptr;
lv_obj_t * chat_back_btn = nullptr;
lv_obj_t * chat_wifi_gear_btn = nullptr;
lv_obj_t * chat_header_bar = nullptr;
lv_obj_t * chat_time_label = nullptr;
lv_obj_t * chat_wifi_label = nullptr;
lv_timer_t * g_chatHeaderTimer = nullptr;

// Refreshes the header's clock + WiFi status. Called on a timer and once
// right after the header is built.
void updateChatHeader(lv_timer_t *timer) {
  if (!chat_time_label || !chat_wifi_label) return;

  time_t now = time(nullptr);
  if (now > 1000000000) { // NTP-synced (after year 2001)
    struct tm *ti = localtime(&now);
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", ti->tm_hour, ti->tm_min);
    lv_label_set_text(chat_time_label, buf);
  } else {
    lv_label_set_text(chat_time_label, "--:--");
  }

  bool online = (WiFi.status() == WL_CONNECTED);
  lv_label_set_text(chat_wifi_label, online ? LV_SYMBOL_WIFI " Online" : LV_SYMBOL_CLOSE " Offline");
  lv_obj_set_style_text_color(chat_wifi_label, online ? lv_color_hex(0x22c55e) : lv_color_hex(0xef4444), 0);
}

// Two layouts: "normal" (header + reply box + input row + Send button) and
// "typing" (header + reply box hidden, input box moved to the very top and
// widened to fill the space Send normally occupies - Send is hidden since
// the keyboard's own Enter/checkmark key submits - and the keyboard fills
// essentially the rest of the screen).
void setChatTypingMode(bool typing) {
  if (typing) {
    lv_obj_add_flag(chat_header_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(chat_response_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(chat_send_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(chat_back_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(chat_wifi_gear_btn, LV_OBJ_FLAG_HIDDEN);
    // Input box is DISPLAY ONLY while typing (T9 writes into it directly -
    // see t9KeypadEventCb - you never need to tap it here), so unlike the
    // back/gear buttons it's fine for it to sit up in the confirmed touch
    // dead zone (y<110 - see runTouchCalibration()'s P1Y): big and visible
    // at the top, full width.
    lv_obj_set_size(chat_input_ta, 460, 100);
    lv_obj_align(chat_input_ta, LV_ALIGN_TOP_MID, 0, 8);
    // Back/gear DO need to be tappable, so they still can't go in the dead
    // zone - but instead of a horizontal row (which cost the keypad
    // height), they're a tall vertical strip beside the keypad (split into
    // two stacked buttons - see createChatScreen()), costing width instead.
    // y=113 is the same dead-zone boundary as before; together they span
    // the full safe area down to y=315, all of which the keypad used to
    // have to share with a separate row above it.
    lv_obj_set_size(chat_kb, 380, 202);
    lv_obj_align(chat_kb, LV_ALIGN_TOP_LEFT, 10, 113);
    lv_obj_clear_flag(chat_kb, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_clear_flag(chat_header_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(chat_response_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(chat_send_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(chat_back_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(chat_wifi_gear_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(chat_input_ta, 340, 40);
    lv_obj_align(chat_input_ta, LV_ALIGN_TOP_LEFT, 10, 270);
    lv_obj_align(chat_send_btn, LV_ALIGN_TOP_RIGHT, -10, 270);
    lv_obj_add_flag(chat_kb, LV_OBJ_FLAG_HIDDEN);
  }
}

// Back-to-front-page button (typing mode only, to the left of the input
// box): cancels typing without sending, just like defocusing used to before
// that got tied to Send-only (see the note on chatTextareaEventCb).
void backToChatBtnEventCb(lv_event_t *e) {
  setChatTypingMode(false);
}

// fwd decls - defined later (WiFi manager section / this function itself),
// used by the gear button below before their real definitions are in scope.
void createChatScreen();
bool runWifiSetupFlow();

// Gear button next to Back in the T9 keypad's side strip: jump straight
// into the WiFi setup screens (scan/select/type password) from the chat
// screen at any time, not just when there's no working connection at boot.
// Set here, actually acted on from loop() - see the comment there for why.
bool g_wifiSetupRequested = false;

void changeWifiFromChatBtnEventCb(lv_event_t *e) {
  g_wifiSetupRequested = true;
}

// Set once a conversation is underway; sending it back as
// previous_interaction_id tells Gemini's Interactions API to continue from
// that point using history it already has server-side, so the ESP32 never
// has to store/resend the growing conversation itself (crucial on 320KB of
// RAM - a client-side "contents" history array would eventually blow that).
String g_previousInteractionId = "";

// Ask Gemini `prompt` (continuing the running conversation, if any) and
// return its reply text (or an error string).
String queryGemini(const String &prompt) {
  if (WiFi.status() != WL_CONNECTED) {
    return "No WiFi connection";
  }

  WiFiClientSecure client;
  client.setInsecure(); // skip TLS cert validation - acceptable for a hobby project
  client.setTimeout(20000); // ms - TLS handshake alone can take a few sec on ESP32

  HTTPClient http;
  const String url = "https://generativelanguage.googleapis.com/v1beta/interactions";
  http.begin(client, url);
  http.setTimeout(20000);        // ms - default is too short for Gemini's response time
  http.setConnectTimeout(20000); // ms
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-goog-api-key", GEMINI_API_KEY);
  http.addHeader("Api-Revision", "2026-05-20"); // per Gemini's Interactions API quickstart

  JsonDocument reqDoc;
  reqDoc["model"] = "gemini-flash-latest";
  reqDoc["input"] = prompt;
  if (g_previousInteractionId.length() > 0) {
    reqDoc["previous_interaction_id"] = g_previousInteractionId;
  }
  String reqBody;
  serializeJson(reqDoc, reqBody);

  String result = "Error contacting Gemini";
  int httpCode = http.POST(reqBody);

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();

    // Only keep the fields we actually need - keeps parsing cheap on 320KB
    // RAM. "steps" is an array of {type, content:[{type,text}]} entries
    // (user_input, model_output, ...); index 0 is ArduinoJson's filter
    // template applied to every array element, not "only element 0".
    JsonDocument filter;
    filter["id"] = true;
    filter["steps"][0]["type"] = true;
    filter["steps"][0]["content"][0]["type"] = true;
    filter["steps"][0]["content"][0]["text"] = true;

    JsonDocument doc;
    DeserializationError err =
        deserializeJson(doc, payload, DeserializationOption::Filter(filter));
    if (!err) {
      const char *newId = doc["id"];
      if (newId) g_previousInteractionId = String(newId);

      for (JsonObject step : doc["steps"].as<JsonArray>()) {
        const char *stepType = step["type"];
        if (!stepType || strcmp(stepType, "model_output") != 0) continue;
        for (JsonObject part : step["content"].as<JsonArray>()) {
          const char *partType = part["type"];
          if (partType && strcmp(partType, "text") == 0) {
            const char *text = part["text"];
            if (text) result = String(text);
          }
        }
      }
    } else {
      result = "Parse error";
      Serial.println(err.c_str());
    }
  } else {
    result = "HTTP error: " + String(httpCode);
    Serial.println(http.getString());
  }

  http.end();
  return result;
}

void sendChatMessage(lv_event_t *e) {
  if (!chat_input_ta || !chat_response_label) return;

  const char *prompt = lv_textarea_get_text(chat_input_ta);
  if (strlen(prompt) == 0) return;

  String promptCopy = prompt; // queryGemini() clears the textarea below
  setChatTypingMode(false); // reveal the reply box + hide keyboard again
  lv_label_set_text(chat_response_label, "Thinking...");
  lv_refr_now(NULL); // paint "Thinking..." before the blocking HTTP call

  String reply = queryGemini(promptCopy);

  lv_label_set_text(chat_response_label, reply.c_str());
  lv_textarea_set_text(chat_input_ta, "");
}

// Show/hide the keyboard as the textarea gains/loses focus, and treat the
// keyboard's Enter key the same as tapping Send.
// The normal-mode button next to the input box: a keyboard icon that opens
// typing mode (tapping the input box itself does the same via FOCUSED
// below, this is just a more obviously-tappable/discoverable entry point).
// Actually sending a message happens via the T9 keypad's own checkmark key.
void openKeyboardBtnEventCb(lv_event_t *e) {
  lv_obj_add_state(chat_input_ta, LV_STATE_FOCUSED); // so the cursor shows
  setChatTypingMode(true);
}

void chatTextareaEventCb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_FOCUSED) {
    setChatTypingMode(true);
  }
  // Deliberately NOT reacting to LV_EVENT_DEFOCUSED here: tapping any T9
  // key on the keypad shifts LVGL's focus away from this textarea too,
  // which fired this same event and exited typing mode on every keypress
  // instead of only on Send. Typing mode now only ends via sendChatMessage()
  // (Send button or the T9 keypad's Send/checkmark key).
}

// ---------------------------------------------------------------------------
// T9-style multi-tap keypad (old feature-phone style): a 4x3 grid instead of
// a QWERTY layout, so each button is huge (~153x65px vs ~40-90px on the
// QWERTY attempts before this). Built on a plain lv_btnmatrix rather than
// lv_keyboard - lv_keyboard always inserts a button's literal label text
// into the bound textarea on click, which can't express "tap twice quickly
// to cycle to the next letter", so button presses are handled entirely by
// hand in t9KeypadEventCb() below instead.
// ---------------------------------------------------------------------------

// Grid order: 1 2 3 / 4 5 6 / 7 8 9 / <BACKSPACE> 0 <SHIFT> <SEND>. Each
// entry is every character that digit's key cycles through on repeated taps
// within T9_CYCLE_TIMEOUT_MS (last char in each string is the digit itself,
// so you can still reach a literal digit by tapping through the whole
// cycle). NULL entries (backspace/shift/send) are handled as special cases,
// not cycled. "-" lives on the 1 key alongside the other punctuation rather
// than getting its own key - the grid has no free slot for it, and it's
// mainly needed for WiFi passwords (this map is shared with wifi_pw_kb),
// not everyday chat typing.
static const char * T9_CYCLES[13] = {
  ".,!?-1", "abc2", "def3",
  "ghi4",   "jkl5", "mno6",
  "pqrs7",  "tuv8", "wxyz9",
  NULL,     " 0",   NULL,    NULL,
};
static const int T9_BACKSPACE_IDX = 9;
static const int T9_SHIFT_IDX = 11;
static const int T9_SEND_IDX = 12;
static const uint32_t T9_CYCLE_TIMEOUT_MS = 600;

// Each button shows its digit and letters on two lines, e.g. "2\nabc" -
// safe to embed a literal newline inside a label like this because
// lv_btnmatrix only treats an array element that IS "\n" (via strcmp) as a
// row break, not one that merely contains one (confirmed in lv_btnmatrix.c).
// Two variants so the on-screen letters actually flip case when Shift is
// toggled (not just the Shift key's own highlight) - t9KeypadEventCb() /
// wifiPwKeypadEventCb() swap the active widget's map via
// lv_btnmatrix_set_map() when Shift is pressed. Typed characters were
// always cased correctly even before this (see the ch-'a'+'A' logic below);
// only the button labels were stuck lowercase.
static const char * chat_t9_map_lower[] = {
  "1\n.,!?-", "2\nabc", "3\ndef", "\n",
  "4\nghi",   "5\njkl", "6\nmno", "\n",
  "7\npqrs",  "8\ntuv", "9\nwxyz", "\n",
  LV_SYMBOL_BACKSPACE, "0\n_", LV_SYMBOL_UP, LV_SYMBOL_OK, "",
};
static const char * chat_t9_map_upper[] = {
  "1\n.,!?-", "2\nABC", "3\nDEF", "\n",
  "4\nGHI",   "5\nJKL", "6\nMNO", "\n",
  "7\nPQRS",  "8\nTUV", "9\nWXYZ", "\n",
  LV_SYMBOL_BACKSPACE, "0\n_", LV_SYMBOL_UP, LV_SYMBOL_OK, "",
};
// Kept as the name used at widget-creation time (lv_btnmatrix_set_map(...,
// chat_t9_map)) - always the lowercase variant; shift swaps away from it.
static const char ** chat_t9_map = chat_t9_map_lower;

int g_t9LastBtnIdx = -1;
int g_t9CyclePos = 0;
uint32_t g_t9LastPressMs = 0;
bool g_t9ShiftOn = false;

void t9KeypadEventCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
  lv_obj_t *btnm = lv_event_get_target(e);
  uint16_t idx = lv_btnmatrix_get_selected_btn(btnm);
  if (idx == LV_BTNMATRIX_BTN_NONE) return;

  if (idx == T9_BACKSPACE_IDX) {
    lv_textarea_del_char(chat_input_ta);
    g_t9LastBtnIdx = -1;
    return;
  }
  if (idx == T9_SEND_IDX) {
    g_t9LastBtnIdx = -1;
    sendChatMessage(NULL);
    return;
  }
  if (idx == T9_SHIFT_IDX) {
    // Persistent toggle (not one-shot) - stays on until tapped again, shown
    // both by the Shift key's own CHECKED highlight and by swapping the
    // whole map so the letter buttons visibly read ABC instead of abc.
    // Doesn't touch g_t9LastBtnIdx, so it doesn't interrupt whatever
    // letter-cycle was in progress.
    g_t9ShiftOn = !g_t9ShiftOn;
    lv_btnmatrix_set_map(btnm, g_t9ShiftOn ? chat_t9_map_upper : chat_t9_map_lower);
    lv_btnmatrix_set_btn_ctrl(btnm, idx, LV_BTNMATRIX_CTRL_CHECKED);
    if (!g_t9ShiftOn) lv_btnmatrix_clear_btn_ctrl(btnm, idx, LV_BTNMATRIX_CTRL_CHECKED);
    return;
  }

  const char *cycle = T9_CYCLES[idx];
  if (!cycle) return;
  int cycleLen = (int)strlen(cycle);

  uint32_t now = millis();
  bool cyclingSameKey = (idx == g_t9LastBtnIdx) && (now - g_t9LastPressMs < T9_CYCLE_TIMEOUT_MS);

  if (cyclingSameKey) {
    lv_textarea_del_char(chat_input_ta); // replace the last candidate letter
    g_t9CyclePos = (g_t9CyclePos + 1) % cycleLen;
  } else {
    g_t9CyclePos = 0; // different key, or timed out - start a new letter
  }

  char ch = cycle[g_t9CyclePos];
  if (g_t9ShiftOn && ch >= 'a' && ch <= 'z') ch = ch - 'a' + 'A';
  lv_textarea_add_char(chat_input_ta, ch);
  g_t9LastBtnIdx = idx;
  g_t9LastPressMs = now;
}

// Builds the entire (only) screen: reply area on top, input box + Send
// button below it, and an lv_keyboard that pops up when the input box is
// focused. Called once from setup().
void createChatScreen() {
  lv_obj_clean(lv_scr_act());
  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x0f172a), 0);

  // Header bar: clock, title, WiFi status (replaces a battery indicator -
  // this board has no battery). Hidden while typing to reclaim space for
  // the input box + keyboard.
  chat_header_bar = lv_obj_create(lv_scr_act());
  lv_obj_set_size(chat_header_bar, 480, 28);
  lv_obj_set_pos(chat_header_bar, 0, 0);
  lv_obj_set_style_bg_color(chat_header_bar, lv_color_hex(0x0f172a), 0);
  lv_obj_set_style_border_width(chat_header_bar, 0, 0);
  lv_obj_set_style_radius(chat_header_bar, 0, 0);
  lv_obj_clear_flag(chat_header_bar, LV_OBJ_FLAG_SCROLLABLE);

  chat_time_label = lv_label_create(chat_header_bar);
  lv_label_set_text(chat_time_label, "--:--");
  lv_obj_set_style_text_color(chat_time_label, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_text_font(chat_time_label, &lv_font_montserrat_14, 0);
  lv_obj_align(chat_time_label, LV_ALIGN_LEFT_MID, 8, 0);

  lv_obj_t *title_label = lv_label_create(chat_header_bar);
  lv_label_set_text(title_label, "AI Assistant");
  lv_obj_set_style_text_color(title_label, lv_color_hex(0x22c55e), 0);
  lv_obj_set_style_text_font(title_label, &lv_font_montserrat_14, 0);
  lv_obj_align(title_label, LV_ALIGN_CENTER, 0, 0);

  chat_wifi_label = lv_label_create(chat_header_bar);
  lv_label_set_text(chat_wifi_label, LV_SYMBOL_CLOSE " Offline");
  lv_obj_set_style_text_font(chat_wifi_label, &lv_font_montserrat_14, 0);
  lv_obj_align(chat_wifi_label, LV_ALIGN_RIGHT_MID, -8, 0);

  updateChatHeader(NULL);                                     // paint real state immediately
  if (g_chatHeaderTimer) lv_timer_del(g_chatHeaderTimer);      // don't stack a duplicate on repeat visits
  g_chatHeaderTimer = lv_timer_create(updateChatHeader, 2000, NULL);

  // Reply area - fills essentially the rest of the screen (input row is
  // pinned to the bottom below it). Hidden while typing (see
  // setChatTypingMode) so the input box + keyboard can take over instead.
  chat_response_box = lv_obj_create(lv_scr_act());
  lv_obj_set_size(chat_response_box, 460, 227);
  lv_obj_align(chat_response_box, LV_ALIGN_TOP_MID, 0, 33);
  lv_obj_set_style_bg_color(chat_response_box, lv_color_hex(0x1e293b), 0);
  lv_obj_set_style_border_width(chat_response_box, 1, 0);
  lv_obj_set_style_border_color(chat_response_box, lv_color_hex(0x334155), 0);
  lv_obj_set_style_radius(chat_response_box, 8, 0);

  chat_response_label = lv_label_create(chat_response_box);
  lv_label_set_long_mode(chat_response_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(chat_response_label, 430);
  lv_label_set_text(chat_response_label, "Ask me anything!");
  lv_obj_set_style_text_color(chat_response_label, lv_color_hex(0xffffff), 0);
  lv_obj_set_style_text_font(chat_response_label, &lv_font_montserrat_14, 0);
  lv_obj_align(chat_response_label, LV_ALIGN_TOP_LEFT, 5, 5);

  // Back-to-front button (typing mode only - hidden here, shown by
  // setChatTypingMode). Sits to the left of the input box.
  // Right-side strip split into two stacked buttons: gear on top to jump
  // into WiFi setup at any time, Back below to cancel typing (same total
  // footprint as the old single button - still clear of the dead zone).
  chat_wifi_gear_btn = lv_btn_create(lv_scr_act());
  lv_obj_set_size(chat_wifi_gear_btn, 70, 97);
  lv_obj_align(chat_wifi_gear_btn, LV_ALIGN_TOP_RIGHT, -10, 113);
  lv_obj_add_event_cb(chat_wifi_gear_btn, changeWifiFromChatBtnEventCb, LV_EVENT_CLICKED, NULL);
  lv_obj_set_style_radius(chat_wifi_gear_btn, 16, 0);
  lv_obj_set_style_bg_color(chat_wifi_gear_btn, lv_color_hex(0x334155), 0);
  lv_obj_t *gear_label = lv_label_create(chat_wifi_gear_btn);
  lv_label_set_text(gear_label, LV_SYMBOL_SETTINGS "\nWiFi");
  lv_obj_set_style_text_align(gear_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(gear_label);
  lv_obj_add_flag(chat_wifi_gear_btn, LV_OBJ_FLAG_HIDDEN);

  chat_back_btn = lv_btn_create(lv_scr_act());
  lv_obj_set_size(chat_back_btn, 70, 97);
  lv_obj_align(chat_back_btn, LV_ALIGN_TOP_RIGHT, -10, 218); // 113 + 97 + 8 gap
  lv_obj_add_event_cb(chat_back_btn, backToChatBtnEventCb, LV_EVENT_CLICKED, NULL);
  lv_obj_set_style_radius(chat_back_btn, 16, 0);
  lv_obj_set_style_bg_color(chat_back_btn, lv_color_hex(0x334155), 0);
  lv_obj_t *back_label = lv_label_create(chat_back_btn);
  lv_label_set_text(back_label, LV_SYMBOL_LEFT "\nBack"); // two lines - fits a tall narrow button
  lv_obj_set_style_text_align(back_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(back_label);
  lv_obj_add_flag(chat_back_btn, LV_OBJ_FLAG_HIDDEN);

  // Input row: textarea + Send button
  chat_input_ta = lv_textarea_create(lv_scr_act());
  lv_obj_set_size(chat_input_ta, 340, 40);
  lv_obj_align(chat_input_ta, LV_ALIGN_TOP_LEFT, 10, 270);
  lv_textarea_set_one_line(chat_input_ta, true);
  lv_textarea_set_placeholder_text(chat_input_ta, "Type a message...");
  lv_obj_add_event_cb(chat_input_ta, chatTextareaEventCb, LV_EVENT_ALL, NULL);
  lv_obj_set_style_radius(chat_input_ta, 16, 0);
  lv_obj_set_style_border_width(chat_input_ta, 2, 0);
  lv_obj_set_style_border_color(chat_input_ta, lv_color_hex(0x22c55e), 0);
  lv_obj_set_style_bg_color(chat_input_ta, lv_color_hex(0x1e293b), 0);
  lv_obj_set_style_text_color(chat_input_ta, lv_color_hex(0xffffff), 0);

  chat_send_btn = lv_btn_create(lv_scr_act());
  lv_obj_set_size(chat_send_btn, 100, 40);
  lv_obj_align(chat_send_btn, LV_ALIGN_TOP_RIGHT, -10, 270);
  lv_obj_add_event_cb(chat_send_btn, openKeyboardBtnEventCb, LV_EVENT_CLICKED, NULL);
  lv_obj_set_style_radius(chat_send_btn, 16, 0);
  lv_obj_set_style_bg_color(chat_send_btn, lv_color_hex(0x22c55e), 0); // match the green accent theme
  lv_obj_t *send_label = lv_label_create(chat_send_btn);
  lv_label_set_text(send_label, LV_SYMBOL_KEYBOARD);
  lv_obj_center(send_label);

  // T9 keypad, hidden until the textarea is focused. setChatTypingMode()
  // resizes/repositions it (and the input row) each time it's shown/hidden.
  // Styled as a rounded bordered "card" to match the input box. A plain
  // lv_btnmatrix, not lv_keyboard - see t9KeypadEventCb() for why.
  chat_kb = lv_btnmatrix_create(lv_scr_act());
  lv_btnmatrix_set_map(chat_kb, chat_t9_map);
  lv_obj_add_event_cb(chat_kb, t9KeypadEventCb, LV_EVENT_VALUE_CHANGED, NULL);
  // Outer "card" background - transparent/borderless so only the individual
  // key outlines below show, matching the reference (each key has its own
  // outline on a plain dark background, not one big bordered box).
  lv_obj_set_style_bg_opa(chat_kb, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(chat_kb, 0, 0);
  lv_obj_set_style_pad_all(chat_kb, 4, 0);
  lv_obj_set_style_pad_row(chat_kb, 6, 0);
  lv_obj_set_style_pad_column(chat_kb, 6, 0);
  // Per-key styling (LV_PART_ITEMS = the individual buttons, not the
  // container) - dark fill, thin green outline, rounded corners.
  lv_obj_set_style_radius(chat_kb, 10, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(chat_kb, lv_color_hex(0x1e293b), LV_PART_ITEMS);
  lv_obj_set_style_border_width(chat_kb, 1, LV_PART_ITEMS);
  lv_obj_set_style_border_color(chat_kb, lv_color_hex(0x22c55e), LV_PART_ITEMS);
  lv_obj_set_style_text_color(chat_kb, lv_color_hex(0xffffff), LV_PART_ITEMS);
  // Shift key's "on" indicator - lv_btnmatrix's own CHECKED state (toggled
  // in t9KeypadEventCb()), styled as a solid green fill so it's obviously
  // different from the other keys' dark fill while active.
  lv_obj_set_style_bg_color(chat_kb, lv_color_hex(0x22c55e), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_text_color(chat_kb, lv_color_hex(0x0f172a), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_add_flag(chat_kb, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------------------
// On-device WiFi manager: scan for networks, pick one by tapping, type its
// password on the same T9 keypad the chat screen uses, and save the result
// so it's remembered across reboots/reflashes - no more hardcoding
// SSID/password in secrets.h and reflashing to change networks. Runs once
// at boot, blocking (same lv_timer_handler()+delay() polling pattern as
// runTouchCalibration()), before the chat screen is built. Layout reuses
// the exact coordinates already proven safe on this panel's touch dead
// zone (see setChatTypingMode()'s comments).
//
// Two storage tiers:
//  - NVS (Preferences) - always available, holds exactly one network. The
//    original mechanism, kept as the fallback for boards with no SD card
//    inserted (or as a safety net if the SD write below fails).
//  - TF/micro-SD card - the board's TF slot (see docs/pcb-layout.jpg: TF_CS
//    IO5, MOSI IO23, MISO IO19, CLK IO18 - the ESP32's default VSPI pins,
//    so no custom SPI.begin() is needed) holds up to
//    MAX_SAVED_WIFI_NETWORKS entries as plain "ssid,password" lines in
//    /wifi_networks.csv - closer to how a phone/laptop remembers many
//    networks. NOTE: stored in plain text on removable media - anyone who
//    pulls the card can read every saved password on a PC. Accepted
//    tradeoff for this project; don't reuse this pattern somewhere that
//    needs real credential protection.
// ---------------------------------------------------------------------------

Preferences g_wifiPrefs;

bool loadSavedWifi(String &ssid, String &pass) {
  g_wifiPrefs.begin("wifi", true); // read-only
  ssid = g_wifiPrefs.getString("ssid", "");
  pass = g_wifiPrefs.getString("pass", "");
  g_wifiPrefs.end();
  return ssid.length() > 0;
}

void saveWifi(const String &ssid, const String &pass) {
  g_wifiPrefs.begin("wifi", false);
  g_wifiPrefs.putString("ssid", ssid);
  g_wifiPrefs.putString("pass", pass);
  g_wifiPrefs.end();
}

#define TF_CS 5
#define WIFI_CSV_PATH "/wifi_networks.csv"
#define MAX_SAVED_WIFI_NETWORKS 8
bool g_sdReady = false;

struct WifiEntry { String ssid; String pass; };

// Reads every saved entry from the SD card into out[] (caller-provided,
// size >= maxCount). Returns how many were found - 0 if the SD card isn't
// ready or the file doesn't exist yet, which callers treat as "no SD
// entries" rather than an error (falls back to the NVS single entry).
int sdLoadWifiNetworks(WifiEntry out[], int maxCount) {
  if (!g_sdReady) return 0;
  File f = SD.open(WIFI_CSV_PATH, FILE_READ);
  if (!f) return 0;
  int count = 0;
  while (f.available() && count < maxCount) {
    String line = f.readStringUntil('\n');
    line.trim(); // drop trailing \r left by readStringUntil('\n') on CRLF lines
    if (line.length() == 0) continue;
    int comma = line.indexOf(',');
    if (comma < 0) continue;
    out[count].ssid = line.substring(0, comma);
    out[count].pass = line.substring(comma + 1);
    count++;
  }
  f.close();
  return count;
}

// Adds/updates one entry and rewrites the whole file. Simplest correct
// approach for a list this small (<= MAX_SAVED_WIFI_NETWORKS) written only
// on a successful new-network connect, not worth a more clever in-place
// edit. SD.open(..., FILE_WRITE) on the ESP32 core APPENDS rather than
// truncating, so the old file is removed first.
void sdSaveWifiNetwork(const String &ssid, const String &pass) {
  if (!g_sdReady) return;
  WifiEntry entries[MAX_SAVED_WIFI_NETWORKS];
  int count = sdLoadWifiNetworks(entries, MAX_SAVED_WIFI_NETWORKS);

  int existing = -1;
  for (int i = 0; i < count; i++) {
    if (entries[i].ssid == ssid) { existing = i; break; }
  }
  if (existing >= 0) {
    entries[existing].pass = pass;
  } else if (count < MAX_SAVED_WIFI_NETWORKS) {
    entries[count].ssid = ssid;
    entries[count].pass = pass;
    count++;
  } else {
    // Full - drop the oldest (front of the list), append the newest.
    for (int i = 1; i < MAX_SAVED_WIFI_NETWORKS; i++) entries[i - 1] = entries[i];
    entries[MAX_SAVED_WIFI_NETWORKS - 1] = {ssid, pass};
  }

  SD.remove(WIFI_CSV_PATH);
  File f = SD.open(WIFI_CSV_PATH, FILE_WRITE);
  if (!f) {
    Serial.println("SD: failed to open " WIFI_CSV_PATH " for writing");
    return;
  }
  for (int i = 0; i < count; i++) {
    f.print(entries[i].ssid);
    f.print(',');
    f.println(entries[i].pass);
  }
  f.close();
}

// Saves to both tiers: the SD list (no-op if no card) so multiple networks
// are remembered, and NVS (always) so there's still a working fallback on
// boards without a card inserted.
void persistWifiCredential(const String &ssid, const String &pass) {
  sdSaveWifiNetwork(ssid, pass);
  saveWifi(ssid, pass);
}

// Tries every SD-saved network that's actually in range (a fresh scan,
// matched by SSID), strongest signal first - phone/laptop-like "best known
// network wins" rather than just file order. Falls back to the single
// NVS-saved network if the SD card isn't ready, its list is empty, or none
// of its entries were in range. Used both at boot and by the chat screen's
// "Cancel" (reconnect) button.
bool connectToAnySavedWifi(lv_obj_t *statusLabel) {
  WifiEntry entries[MAX_SAVED_WIFI_NETWORKS];
  int count = sdLoadWifiNetworks(entries, MAX_SAVED_WIFI_NETWORKS);

  if (count > 0) {
    if (statusLabel) {
      lv_label_set_text(statusLabel, "Scanning for saved networks...");
      lv_refr_now(NULL);
    }
    int n = WiFi.scanNetworks();
    int bestEntry = -1, bestRssi = -1000;
    for (int i = 0; i < n; i++) {
      for (int j = 0; j < count; j++) {
        if (WiFi.SSID(i) == entries[j].ssid && WiFi.RSSI(i) > bestRssi) {
          bestRssi = WiFi.RSSI(i);
          bestEntry = j;
        }
      }
    }
    if (bestEntry >= 0 && tryConnectWifi(entries[bestEntry].ssid, entries[bestEntry].pass, statusLabel)) {
      return true;
    }
    // None of the SD entries were in range (or none connected) - fall
    // through to the NVS single entry as a last resort.
  }

  String ssid, pass;
  if (loadSavedWifi(ssid, pass)) {
    return tryConnectWifi(ssid, pass, statusLabel);
  }
  return false;
}

// Blocking connect attempt (up to ~10s), updating statusLabel (if given)
// as it goes. Returns true on success.
bool tryConnectWifi(const String &ssid, const String &pass, lv_obj_t *statusLabel) {
  Serial.printf("Connecting to WiFi: %s\r\n", ssid.c_str());
  WiFi.disconnect(true);
  delay(1000); // full radio power-cycle from disconnect(true) needs this long to settle
  WiFi.mode(WIFI_STA);
  delay(100);
  WiFi.begin(ssid.c_str(), pass.c_str());

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    attempts++;
    if (statusLabel) {
      String dots = "";
      for (int i = 0; i < (attempts % 4); i++) dots += ".";
      lv_label_set_text_fmt(statusLabel, "Connecting%s", dots.c_str());
      lv_refr_now(NULL);
    }
  }
  bool ok = (WiFi.status() == WL_CONNECTED);
  Serial.println(ok ? "WiFi connected!" : "WiFi connect failed.");
  return ok;
}

// Set by the list/password views' button handlers; polled by the blocking
// runWifiSetupFlow() loop in setup() to know when to stop.
bool g_wifiSetupDone = false;
bool g_wifiSetupConnected = false;

void gt911_int_(); // fwd decl - defined near the bottom of the file

// --- Single combined setup screen -------------------------------------------
// Network list and password entry are two VIEWS on the same screen, toggled
// by show/hide - not separate screens rebuilt via lv_obj_clean(). Originally
// these were two lv_obj_clean()-rebuilt screens; after the WiFi scan, touch
// stopped registering at all (zero raw reads, not just missed clicks) and
// neither re-running the GT911 init nor other fixes recovered it, so this
// cuts the number of full screen tear-downs/rebuilds during the flow down
// to just one (right when the flow starts), which also means backing out of
// password entry to pick a different network doesn't need a re-scan.
// ---------------------------------------------------------------------------

// Network picker: a wide label (column 1) shows the currently-selected
// SSID; Up/Select/Down (column 2) step through the scan results and
// confirm one. This replaced a per-item clickable list (one button per
// network in a shared container) that rendered fine but wouldn't reliably
// register clicks - Up/Select/Down are the same kind of standalone lv_btn
// directly on the screen as Skip/Cancel/gear/T9 keys, which have all
// worked reliably, so this sidesteps whatever was specific to the list.
// Column 1 is a real multi-row list now: WIFI_LIST_VISIBLE_ROWS row widgets
// created once, refreshed in place by updateWifiListRows() as Up/Down move
// the highlight (a sliding window over the scan results - no scrolling, no
// per-row click handlers; rows are display-only, Select confirms the
// highlighted one). Rows are plain lv_obj + label children of the box,
// which is safe: the earlier unresponsive-list problem turned out to be the
// nested lv_timer_handler() call (see loop()), not child widgets.
#define WIFI_LIST_VISIBLE_ROWS 7
lv_obj_t * wifi_list_title = nullptr;    // "Select a WiFi network" - hide/show target
lv_obj_t * wifi_current_box = nullptr;   // bordered container - hide/show target
lv_obj_t * wifi_list_rows[WIFI_LIST_VISIBLE_ROWS] = {nullptr};   // row background objects
lv_obj_t * wifi_list_row_labels[WIFI_LIST_VISIBLE_ROWS] = {nullptr}; // their text labels
lv_obj_t * wifi_up_btn = nullptr;
lv_obj_t * wifi_select_btn = nullptr;
lv_obj_t * wifi_down_btn = nullptr;
lv_obj_t * wifi_list_cancel_btn = nullptr;
lv_obj_t * wifi_list_skip_btn = nullptr;
int g_wifiListIdx = 0;
int g_wifiListCount = 0;

lv_obj_t * wifi_pw_title = nullptr;
lv_obj_t * wifi_pw_ta = nullptr;
lv_obj_t * wifi_pw_status_label = nullptr;
lv_obj_t * wifi_pw_back_btn = nullptr;
lv_obj_t * wifi_pw_symbols_btn = nullptr; // top half of what used to be one tall Back button
lv_obj_t * wifi_pw_kb = nullptr;
String g_wifiSetupSsid = "";

int g_wifiT9LastBtnIdx = -1;
int g_wifiT9CyclePos = 0;
uint32_t g_wifiT9LastPressMs = 0;

// Symbols key: standalone button (not part of the T9 btnmatrix/T9_CYCLES -
// it lives outside the keypad grid, in the space freed up by shrinking
// wifi_pw_back_btn), but cycles on repeated taps the same way a T9 key
// does. Password-oriented set that doesn't already live on T9 key "1"
// (".,!?-") - common special characters password policies ask for.
static const char * WIFI_SYMBOLS_CYCLE = "@#$%^&*-_";
int g_wifiSymCyclePos = 0;
uint32_t g_wifiSymLastPressMs = 0;
bool g_wifiT9ShiftOn = false;

void updateWifiListRows() {
  if (g_wifiListCount <= 0) {
    for (int i = 0; i < WIFI_LIST_VISIBLE_ROWS; i++) {
      lv_obj_add_flag(wifi_list_rows[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(wifi_list_rows[0], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(wifi_list_row_labels[0], "No networks found");
    return;
  }

  // Sliding window over the results, keeping the highlighted entry visible
  // (roughly centered once the list is longer than the window).
  int start = g_wifiListIdx - WIFI_LIST_VISIBLE_ROWS / 2;
  if (start > g_wifiListCount - WIFI_LIST_VISIBLE_ROWS) start = g_wifiListCount - WIFI_LIST_VISIBLE_ROWS;
  if (start < 0) start = 0;

  for (int i = 0; i < WIFI_LIST_VISIBLE_ROWS; i++) {
    int netIdx = start + i;
    if (netIdx >= g_wifiListCount) {
      lv_obj_add_flag(wifi_list_rows[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_clear_flag(wifi_list_rows[i], LV_OBJ_FLAG_HIDDEN);
    bool isOpen = (WiFi.encryptionType(netIdx) == WIFI_AUTH_OPEN);
    lv_label_set_text_fmt(wifi_list_row_labels[i], "%s%s",
                           isOpen ? "" : LV_SYMBOL_CLOSE " ",
                           WiFi.SSID(netIdx).c_str());
    bool highlighted = (netIdx == g_wifiListIdx);
    lv_obj_set_style_bg_color(wifi_list_rows[i],
                               highlighted ? lv_color_hex(0x22c55e) : lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_text_color(wifi_list_row_labels[i],
                                 highlighted ? lv_color_hex(0x0f172a) : lv_color_hex(0xffffff), 0);
  }
}

void showWifiListView() {
  lv_obj_clear_flag(wifi_list_title, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(wifi_current_box, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(wifi_up_btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(wifi_select_btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(wifi_down_btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(wifi_list_cancel_btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(wifi_list_skip_btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(wifi_pw_title, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(wifi_pw_ta, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(wifi_pw_status_label, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(wifi_pw_symbols_btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(wifi_pw_back_btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(wifi_pw_kb, LV_OBJ_FLAG_HIDDEN);
}

void showWifiPasswordView(const String &ssid) {
  g_wifiSetupSsid = ssid;
  g_wifiT9LastBtnIdx = -1;
  g_wifiT9ShiftOn = false;
  g_wifiSymCyclePos = 0;
  g_wifiSymLastPressMs = 0;
  lv_btnmatrix_set_map(wifi_pw_kb, chat_t9_map_lower);
  lv_btnmatrix_clear_btn_ctrl(wifi_pw_kb, T9_SHIFT_IDX, LV_BTNMATRIX_CTRL_CHECKED);
  lv_label_set_text_fmt(wifi_pw_title, "Password for: %s", ssid.c_str());
  lv_textarea_set_text(wifi_pw_ta, "");
  lv_label_set_text(wifi_pw_status_label, "");

  // wifi_list_title ("Select a WiFi network") used to be left showing here
  // too - it and wifi_pw_title ("Password for: ...") both sit at the same
  // TOP_MID/y=8 spot, so the two labels rendered stacked on top of each
  // other in this view. Hiding it is the fix.
  lv_obj_add_flag(wifi_list_title, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(wifi_current_box, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(wifi_up_btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(wifi_select_btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(wifi_down_btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(wifi_list_cancel_btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(wifi_list_skip_btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(wifi_pw_title, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(wifi_pw_ta, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(wifi_pw_status_label, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(wifi_pw_symbols_btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(wifi_pw_back_btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(wifi_pw_kb, LV_OBJ_FLAG_HIDDEN);
}

void wifiBackToListBtnEventCb(lv_event_t *e) {
  showWifiListView();
}

// Cycles WIFI_SYMBOLS_CYCLE on repeated taps, same multi-tap-within-timeout
// pattern as the T9 letter keys, just standalone (this button isn't part of
// the btnmatrix/T9_CYCLES - see wifi_pw_symbols_btn's declaration).
void wifiPwSymbolsBtnEventCb(lv_event_t *e) {
  int cycleLen = (int)strlen(WIFI_SYMBOLS_CYCLE);
  uint32_t now = millis();
  bool cycling = (now - g_wifiSymLastPressMs < T9_CYCLE_TIMEOUT_MS) && g_wifiSymLastPressMs != 0;

  if (cycling) {
    lv_textarea_del_char(wifi_pw_ta);
    g_wifiSymCyclePos = (g_wifiSymCyclePos + 1) % cycleLen;
  } else {
    g_wifiSymCyclePos = 0;
  }

  lv_textarea_add_char(wifi_pw_ta, WIFI_SYMBOLS_CYCLE[g_wifiSymCyclePos]);
  g_wifiSymLastPressMs = now;
}

void wifiPwKeypadEventCb(lv_event_t *e) {
  if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
  lv_obj_t *btnm = lv_event_get_target(e);
  uint16_t idx = lv_btnmatrix_get_selected_btn(btnm);
  if (idx == LV_BTNMATRIX_BTN_NONE) return;

  if (idx == T9_BACKSPACE_IDX) {
    lv_textarea_del_char(wifi_pw_ta);
    g_wifiT9LastBtnIdx = -1;
    return;
  }
  if (idx == T9_SEND_IDX) {
    g_wifiT9LastBtnIdx = -1;
    String pass = lv_textarea_get_text(wifi_pw_ta);
    lv_label_set_text(wifi_pw_status_label, "Connecting...");
    lv_refr_now(NULL);
    if (tryConnectWifi(g_wifiSetupSsid, pass, wifi_pw_status_label)) {
      persistWifiCredential(g_wifiSetupSsid, pass);
      g_wifiSetupConnected = true;
      g_wifiSetupDone = true;
    } else {
      lv_label_set_text(wifi_pw_status_label, "Failed - check password, try again");
    }
    return;
  }
  if (idx == T9_SHIFT_IDX) {
    g_wifiT9ShiftOn = !g_wifiT9ShiftOn;
    lv_btnmatrix_set_map(btnm, g_wifiT9ShiftOn ? chat_t9_map_upper : chat_t9_map_lower);
    lv_btnmatrix_set_btn_ctrl(btnm, idx, LV_BTNMATRIX_CTRL_CHECKED);
    if (!g_wifiT9ShiftOn) lv_btnmatrix_clear_btn_ctrl(btnm, idx, LV_BTNMATRIX_CTRL_CHECKED);
    return;
  }

  const char *cycle = T9_CYCLES[idx];
  if (!cycle) return;
  int cycleLen = (int)strlen(cycle);

  uint32_t now = millis();
  bool cyclingSameKey = (idx == g_wifiT9LastBtnIdx) && (now - g_wifiT9LastPressMs < T9_CYCLE_TIMEOUT_MS);

  if (cyclingSameKey) {
    lv_textarea_del_char(wifi_pw_ta);
    g_wifiT9CyclePos = (g_wifiT9CyclePos + 1) % cycleLen;
  } else {
    g_wifiT9CyclePos = 0;
  }

  char ch = cycle[g_wifiT9CyclePos];
  if (g_wifiT9ShiftOn && ch >= 'a' && ch <= 'z') ch = ch - 'a' + 'A';
  lv_textarea_add_char(wifi_pw_ta, ch);
  g_wifiT9LastBtnIdx = idx;
  g_wifiT9LastPressMs = now;
}

void wifiListSkipBtnEventCb(lv_event_t *e) {
  g_wifiSetupConnected = false;
  g_wifiSetupDone = true;
}

// Distinct from Skip: this is for when the setup screen was opened from the
// chat screen's gear button while ALREADY connected (not just at boot with
// nothing working yet) - "cancel" should restore whatever was previously
// connected rather than force offline mode.
void wifiListCancelBtnEventCb(lv_event_t *e) {
  connectToAnySavedWifi(nullptr);
  g_wifiSetupConnected = (WiFi.status() == WL_CONNECTED);
  g_wifiSetupDone = true;
}

void wifiUpBtnEventCb(lv_event_t *e) {
  if (g_wifiListCount <= 0) return;
  g_wifiListIdx = (g_wifiListIdx - 1 + g_wifiListCount) % g_wifiListCount;
  updateWifiListRows();
}

void wifiDownBtnEventCb(lv_event_t *e) {
  if (g_wifiListCount <= 0) return;
  g_wifiListIdx = (g_wifiListIdx + 1) % g_wifiListCount;
  updateWifiListRows();
}

void wifiSelectBtnEventCb(lv_event_t *e) {
  if (g_wifiListCount <= 0) return;
  String ssid = WiFi.SSID(g_wifiListIdx);
  bool isOpen = (WiFi.encryptionType(g_wifiListIdx) == WIFI_AUTH_OPEN);
  if (isOpen) {
    if (tryConnectWifi(ssid, "", nullptr)) {
      persistWifiCredential(ssid, "");
      g_wifiSetupConnected = true;
      g_wifiSetupDone = true;
      return;
    }
    // fall through to password view if an "open" network unexpectedly
    // still needs one (captive portals etc. aren't handled - offer manual entry)
  }
  showWifiPasswordView(ssid);
}

void createWifiSetupScreen() {
  lv_obj_clean(lv_scr_act());
  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x0f172a), 0);

  wifi_list_title = lv_label_create(lv_scr_act());
  lv_label_set_text(wifi_list_title, "Select a WiFi network");
  lv_obj_set_style_text_color(wifi_list_title, lv_color_hex(0x22c55e), 0);
  lv_obj_align(wifi_list_title, LV_ALIGN_TOP_MID, 0, 8);
  lv_refr_now(NULL); // paint the title before the blocking scan below

  // Two columns, all standalone lv_btn/lv_obj widgets directly on the
  // screen (same class of widget as gear/T9 keys, all of which have worked
  // reliably) instead of many small buttons inside one shared container
  // (which rendered fine but wouldn't reliably register clicks):
  //   col 1 (wide, x=10..300): current SSID display, non-interactive
  //   col 2 (x=310..470): Up / Select / Down / Back / Skip, stacked

  // Only fully reset the radio before scanning if we're not already
  // connected. A mid-chat "change WiFi" tap (the gear button) starts out
  // already connected, and unconditionally forcing disconnect(true) + a 1s
  // settle here was one of the suspects for touch going dead afterwards -
  // scanning while connected is a normal supported ESP32 WiFi operation, no
  // need to tear the radio down for it.
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect(true);
    delay(1000); // full radio power-cycle from disconnect(true) needs this long to settle
    WiFi.mode(WIFI_STA);
    delay(100);
  }

  Serial.println("Scanning WiFi networks...");
  int n = WiFi.scanNetworks();
  Serial.printf("scanNetworks() returned %d\r\n", n);
  // ESP32 quirk: the very first scanNetworks() right after a mode/radio
  // change frequently comes back 0 or WIFI_SCAN_FAILED (-2) because the
  // radio hasn't settled yet - a second attempt a moment later almost
  // always succeeds. Without this retry, a bad first scan left the list
  // empty and Up/Down/Select silently no-op'd (they early-return whenever
  // g_wifiListCount <= 0 - see wifiUpBtnEventCb() etc.), which looked like
  // "the buttons don't work" with no way to recover short of Cancel/Skip.
  for (int attempt = 0; n <= 0 && attempt < 2; attempt++) {
    Serial.println("Scan came back empty/failed - retrying...");
    delay(300);
    n = WiFi.scanNetworks();
    Serial.printf("scanNetworks() retry returned %d\r\n", n);
  }
  g_wifiListCount = n > 0 ? n : 0;
  g_wifiListIdx = 0;

  // --- Column 1: the network list (7 visible rows, sliding window,
  // highlighted row = current selection; see updateWifiListRows()).
  // Full height from just under the title down to the screen bottom - the
  // rows are display-only (Up/Down/Select do all the interaction), so
  // unlike the buttons this box is allowed to extend up into the touch
  // dead zone (y<110) without any downside. ---
  wifi_current_box = lv_obj_create(lv_scr_act());
  lv_obj_set_size(wifi_current_box, 290, 280);
  lv_obj_align(wifi_current_box, LV_ALIGN_TOP_LEFT, 10, 35);
  lv_obj_set_style_bg_color(wifi_current_box, lv_color_hex(0x1e293b), 0);
  lv_obj_set_style_border_width(wifi_current_box, 2, 0);
  lv_obj_set_style_border_color(wifi_current_box, lv_color_hex(0x22c55e), 0); // green outline - same theme as input box/T9 keys
  lv_obj_set_style_radius(wifi_current_box, 8, 0);
  lv_obj_set_style_pad_all(wifi_current_box, 4, 0);
  lv_obj_clear_flag(wifi_current_box, LV_OBJ_FLAG_SCROLLABLE);

  // 7 rows x 36px + 6x 3px gaps = 270px inside the 280px box (with padding)
  for (int i = 0; i < WIFI_LIST_VISIBLE_ROWS; i++) {
    wifi_list_rows[i] = lv_obj_create(wifi_current_box);
    lv_obj_set_size(wifi_list_rows[i], 274, 36);
    lv_obj_set_pos(wifi_list_rows[i], 0, i * 39);
    lv_obj_set_style_bg_color(wifi_list_rows[i], lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_border_width(wifi_list_rows[i], 1, 0);
    lv_obj_set_style_border_color(wifi_list_rows[i], lv_color_hex(0x22c55e), 0); // same outline theme as T9 keys
    lv_obj_set_style_radius(wifi_list_rows[i], 6, 0);
    lv_obj_set_style_pad_all(wifi_list_rows[i], 0, 0);
    lv_obj_clear_flag(wifi_list_rows[i], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(wifi_list_rows[i], LV_OBJ_FLAG_CLICKABLE); // display-only; Select confirms

    wifi_list_row_labels[i] = lv_label_create(wifi_list_rows[i]);
    lv_obj_set_style_text_color(wifi_list_row_labels[i], lv_color_hex(0xffffff), 0);
    lv_label_set_long_mode(wifi_list_row_labels[i], LV_LABEL_LONG_DOT);
    lv_obj_set_width(wifi_list_row_labels[i], 258);
    lv_obj_align(wifi_list_row_labels[i], LV_ALIGN_LEFT_MID, 8, 0);
  }

  if (n < 0) {
    for (int i = 1; i < WIFI_LIST_VISIBLE_ROWS; i++) lv_obj_add_flag(wifi_list_rows[i], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(wifi_list_row_labels[0], "Scan failed (code %d)", n);
  } else {
    updateWifiListRows();
  }

  // --- Column 2: Up / Select / Down / Back / Skip, all stacked. 5 buttons
  // in 202px -> 37px each with 4px gaps (5*37 + 4*4 = 201).
  const int32_t WIFI_COL2_X = 310, WIFI_COL2_W = 160;
  const int32_t WIFI_COL2_ROW_H = 37, WIFI_COL2_GAP = 4;
  int32_t col2Y = 113;

  wifi_up_btn = lv_btn_create(lv_scr_act());
  lv_obj_set_size(wifi_up_btn, WIFI_COL2_W, WIFI_COL2_ROW_H);
  lv_obj_align(wifi_up_btn, LV_ALIGN_TOP_LEFT, WIFI_COL2_X, col2Y);
  lv_obj_set_style_radius(wifi_up_btn, 10, 0);
  lv_obj_set_style_bg_color(wifi_up_btn, lv_color_hex(0x1e293b), 0);
  lv_obj_set_style_border_width(wifi_up_btn, 1, 0);
  lv_obj_set_style_border_color(wifi_up_btn, lv_color_hex(0x22c55e), 0); // same outline theme as T9 keys
  lv_obj_add_event_cb(wifi_up_btn, wifiUpBtnEventCb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *up_label = lv_label_create(wifi_up_btn);
  lv_label_set_text(up_label, LV_SYMBOL_UP);
  lv_obj_center(up_label);
  col2Y += WIFI_COL2_ROW_H + WIFI_COL2_GAP;

  wifi_select_btn = lv_btn_create(lv_scr_act());
  lv_obj_set_size(wifi_select_btn, WIFI_COL2_W, WIFI_COL2_ROW_H);
  lv_obj_align(wifi_select_btn, LV_ALIGN_TOP_LEFT, WIFI_COL2_X, col2Y);
  lv_obj_set_style_radius(wifi_select_btn, 10, 0);
  lv_obj_set_style_bg_color(wifi_select_btn, lv_color_hex(0x22c55e), 0);
  lv_obj_add_event_cb(wifi_select_btn, wifiSelectBtnEventCb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *select_label = lv_label_create(wifi_select_btn);
  lv_label_set_text(select_label, "Select");
  lv_obj_center(select_label);
  col2Y += WIFI_COL2_ROW_H + WIFI_COL2_GAP;

  wifi_down_btn = lv_btn_create(lv_scr_act());
  lv_obj_set_size(wifi_down_btn, WIFI_COL2_W, WIFI_COL2_ROW_H);
  lv_obj_align(wifi_down_btn, LV_ALIGN_TOP_LEFT, WIFI_COL2_X, col2Y);
  lv_obj_set_style_radius(wifi_down_btn, 10, 0);
  lv_obj_set_style_bg_color(wifi_down_btn, lv_color_hex(0x1e293b), 0);
  lv_obj_set_style_border_width(wifi_down_btn, 1, 0);
  lv_obj_set_style_border_color(wifi_down_btn, lv_color_hex(0x22c55e), 0); // same outline theme as T9 keys
  lv_obj_add_event_cb(wifi_down_btn, wifiDownBtnEventCb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *down_label = lv_label_create(wifi_down_btn);
  lv_label_set_text(down_label, LV_SYMBOL_DOWN);
  lv_obj_center(down_label);
  col2Y += WIFI_COL2_ROW_H + WIFI_COL2_GAP;

  // Cancel (reconnect to whatever was working before) / Skip (deliberately
  // go offline) - distinct actions, see wifiListCancelBtnEventCb()'s
  // comment for why they can't be the same button.
  wifi_list_cancel_btn = lv_btn_create(lv_scr_act());
  lv_obj_set_size(wifi_list_cancel_btn, WIFI_COL2_W, WIFI_COL2_ROW_H);
  lv_obj_align(wifi_list_cancel_btn, LV_ALIGN_TOP_LEFT, WIFI_COL2_X, col2Y);
  lv_obj_set_style_radius(wifi_list_cancel_btn, 10, 0);
  lv_obj_set_style_bg_color(wifi_list_cancel_btn, lv_color_hex(0x1e293b), 0);
  lv_obj_set_style_border_width(wifi_list_cancel_btn, 1, 0);
  lv_obj_set_style_border_color(wifi_list_cancel_btn, lv_color_hex(0x22c55e), 0); // same outline theme as T9 keys
  lv_obj_add_event_cb(wifi_list_cancel_btn, wifiListCancelBtnEventCb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *cancel_label = lv_label_create(wifi_list_cancel_btn);
  lv_label_set_text(cancel_label, LV_SYMBOL_LEFT " Back");
  lv_obj_center(cancel_label);
  col2Y += WIFI_COL2_ROW_H + WIFI_COL2_GAP;

  wifi_list_skip_btn = lv_btn_create(lv_scr_act());
  lv_obj_set_size(wifi_list_skip_btn, WIFI_COL2_W, WIFI_COL2_ROW_H);
  lv_obj_align(wifi_list_skip_btn, LV_ALIGN_TOP_LEFT, WIFI_COL2_X, col2Y);
  lv_obj_set_style_radius(wifi_list_skip_btn, 10, 0);
  lv_obj_set_style_bg_color(wifi_list_skip_btn, lv_color_hex(0x1e293b), 0);
  lv_obj_set_style_border_width(wifi_list_skip_btn, 1, 0);
  lv_obj_set_style_border_color(wifi_list_skip_btn, lv_color_hex(0x22c55e), 0); // same outline theme as T9 keys
  lv_obj_add_event_cb(wifi_list_skip_btn, wifiListSkipBtnEventCb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *skip_label = lv_label_create(wifi_list_skip_btn);
  lv_label_set_text(skip_label, "Skip");
  lv_obj_center(skip_label);

  // --- Password view - built here but hidden; showWifiPasswordView() and
  // showWifiListView() toggle which view is visible without ever rebuilding
  // either one. Same coordinates as the chat screen's typing mode / the
  // list above (all field-tested clear of the dead zone).
  wifi_pw_title = lv_label_create(lv_scr_act());
  lv_label_set_text(wifi_pw_title, "Password for:");
  lv_obj_set_style_text_color(wifi_pw_title, lv_color_hex(0x22c55e), 0);
  lv_obj_align(wifi_pw_title, LV_ALIGN_TOP_MID, 0, 8);

  wifi_pw_ta = lv_textarea_create(lv_scr_act());
  lv_obj_set_size(wifi_pw_ta, 460, 40);
  lv_obj_align(wifi_pw_ta, LV_ALIGN_TOP_MID, 0, 35);
  lv_textarea_set_one_line(wifi_pw_ta, true);
  lv_textarea_set_password_mode(wifi_pw_ta, false); // shown in plain text, not masked, per request
  lv_textarea_set_placeholder_text(wifi_pw_ta, "Password");
  lv_obj_set_style_radius(wifi_pw_ta, 16, 0);
  lv_obj_set_style_border_width(wifi_pw_ta, 2, 0);
  lv_obj_set_style_border_color(wifi_pw_ta, lv_color_hex(0x22c55e), 0);
  lv_obj_set_style_bg_color(wifi_pw_ta, lv_color_hex(0x1e293b), 0);
  lv_obj_set_style_text_color(wifi_pw_ta, lv_color_hex(0xffffff), 0);

  wifi_pw_status_label = lv_label_create(lv_scr_act());
  lv_label_set_text(wifi_pw_status_label, "");
  lv_obj_set_style_text_color(wifi_pw_status_label, lv_color_hex(0xffffff), 0);
  lv_obj_align(wifi_pw_status_label, LV_ALIGN_TOP_MID, 0, 80);

  // This used to be one 70x202 Back button running the full height of the
  // keypad - way taller than it needed to be for one action. Split into two
  // 70x99 buttons stacked with a 4px gap (99+4+99=202, same footprint):
  // Symbols on top (password-oriented punctuation the T9 cycles don't
  // cover), Back on the bottom.
  wifi_pw_symbols_btn = lv_btn_create(lv_scr_act());
  lv_obj_set_size(wifi_pw_symbols_btn, 70, 99);
  lv_obj_align(wifi_pw_symbols_btn, LV_ALIGN_TOP_RIGHT, -10, 113);
  lv_obj_add_event_cb(wifi_pw_symbols_btn, wifiPwSymbolsBtnEventCb, LV_EVENT_CLICKED, NULL);
  lv_obj_set_style_radius(wifi_pw_symbols_btn, 16, 0);
  lv_obj_set_style_bg_color(wifi_pw_symbols_btn, lv_color_hex(0x1e293b), 0);
  lv_obj_set_style_border_width(wifi_pw_symbols_btn, 1, 0);
  lv_obj_set_style_border_color(wifi_pw_symbols_btn, lv_color_hex(0x22c55e), 0); // same outline theme as T9 keys
  lv_obj_t *symbols_label = lv_label_create(wifi_pw_symbols_btn);
  lv_label_set_text(symbols_label, "@#$\n%^&*");
  lv_obj_set_style_text_align(symbols_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(symbols_label);

  wifi_pw_back_btn = lv_btn_create(lv_scr_act());
  lv_obj_set_size(wifi_pw_back_btn, 70, 99);
  lv_obj_align(wifi_pw_back_btn, LV_ALIGN_TOP_RIGHT, -10, 113 + 99 + 4);
  lv_obj_add_event_cb(wifi_pw_back_btn, wifiBackToListBtnEventCb, LV_EVENT_CLICKED, NULL);
  lv_obj_set_style_radius(wifi_pw_back_btn, 16, 0);
  lv_obj_set_style_bg_color(wifi_pw_back_btn, lv_color_hex(0x1e293b), 0);
  lv_obj_set_style_border_width(wifi_pw_back_btn, 1, 0);
  lv_obj_set_style_border_color(wifi_pw_back_btn, lv_color_hex(0x22c55e), 0); // same outline theme as T9 keys
  lv_obj_t *back_label = lv_label_create(wifi_pw_back_btn);
  lv_label_set_text(back_label, LV_SYMBOL_LEFT "\nBack");
  lv_obj_set_style_text_align(back_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(back_label);

  wifi_pw_kb = lv_btnmatrix_create(lv_scr_act());
  lv_btnmatrix_set_map(wifi_pw_kb, chat_t9_map);
  lv_obj_add_event_cb(wifi_pw_kb, wifiPwKeypadEventCb, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_set_style_bg_opa(wifi_pw_kb, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(wifi_pw_kb, 0, 0);
  lv_obj_set_style_pad_all(wifi_pw_kb, 4, 0);
  lv_obj_set_style_pad_row(wifi_pw_kb, 6, 0);
  lv_obj_set_style_pad_column(wifi_pw_kb, 6, 0);
  lv_obj_set_style_radius(wifi_pw_kb, 10, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(wifi_pw_kb, lv_color_hex(0x1e293b), LV_PART_ITEMS);
  lv_obj_set_style_border_width(wifi_pw_kb, 1, LV_PART_ITEMS);
  lv_obj_set_style_border_color(wifi_pw_kb, lv_color_hex(0x22c55e), LV_PART_ITEMS);
  lv_obj_set_style_text_color(wifi_pw_kb, lv_color_hex(0xffffff), LV_PART_ITEMS);
  // Shift key's "on" indicator - see the matching styling on chat_kb above.
  lv_obj_set_style_bg_color(wifi_pw_kb, lv_color_hex(0x22c55e), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_text_color(wifi_pw_kb, lv_color_hex(0x0f172a), LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_size(wifi_pw_kb, 380, 202);
  lv_obj_align(wifi_pw_kb, LV_ALIGN_TOP_LEFT, 10, 113);

  showWifiListView(); // start on the list view

  // Without this, nothing built above reliably appeared until some later
  // unrelated event happened to trigger a repaint - seen before with the
  // per-item list too. Force it explicitly right after the long blocking
  // WiFi.scanNetworks() call rather than hoping the next natural tick does it.
  lv_obj_invalidate(lv_scr_act());
  lv_refr_now(NULL);
}

// Blocking, like runTouchCalibration(): shows the setup screen and keeps
// LVGL ticking until a network connects successfully or the user skips.
// List <-> password view switching happens inside the button handlers
// above (show/hide, no rebuild) without touching g_wifiSetupDone.
bool runWifiSetupFlow() {
  // createChatScreen() always runs before this (boot fallback or the gear
  // button), so its header timer is always active at this point. Left
  // running, it would keep firing every 2s and writing into
  // chat_time_label/chat_wifi_label - destroyed by the lv_obj_clean() below
  // but the global pointers don't get nulled out, so that write lands on
  // freed memory.
  if (g_chatHeaderTimer) {
    lv_timer_del(g_chatHeaderTimer);
    g_chatHeaderTimer = nullptr;
  }

  g_wifiSetupDone = false;
  g_wifiSetupConnected = false;
  createWifiSetupScreen();
  while (!g_wifiSetupDone) {
    lv_timer_handler();
    delay(15);
  }
  return g_wifiSetupConnected;
}

/* 显示器刷新 */
void my_disp_flush( lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p )
{
  uint32_t w = ( area->x2 - area->x1 + 1 );
  uint32_t h = ( area->y2 - area->y1 + 1 );

  tft.startWrite();
  tft.setAddrWindow( area->x1, area->y1, w, h );
  tft.pushColors( ( uint16_t * )&color_p->full, w * h, true );
  tft.endWrite();

  lv_disp_flush_ready( disp );
}
//_______________________

void gt911_test(void)
{
  uint8_t i = 0;

}

const uint8_t GT9111_CFG_TBL[] =
{
  0X60, 0X40, 0X01, 0XE0, 0X01, 0X05, 0X35, 0X00, 0X02, 0X08,
  0X1E, 0X08, 0X50, 0X3C, 0X0F, 0X05, 0X00, 0X00, 0XFF, 0X67,
  0X50, 0X00, 0X00, 0X18, 0X1A, 0X1E, 0X14, 0X89, 0X28, 0X0A,
  0X30, 0X2E, 0XBB, 0X0A, 0X03, 0X00, 0X00, 0X02, 0X33, 0X1D,
  0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X32, 0X00, 0X00,
  0X2A, 0X1C, 0X5A, 0X94, 0XC5, 0X02, 0X07, 0X00, 0X00, 0X00,
  0XB5, 0X1F, 0X00, 0X90, 0X28, 0X00, 0X77, 0X32, 0X00, 0X62,
  0X3F, 0X00, 0X52, 0X50, 0X00, 0X52, 0X00, 0X00, 0X00, 0X00,
  0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00,
  0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X0F,
  0X0F, 0X03, 0X06, 0X10, 0X42, 0XF8, 0X0F, 0X14, 0X00, 0X00,
  0X00, 0X00, 0X1A, 0X18, 0X16, 0X14, 0X12, 0X10, 0X0E, 0X0C,
  0X0A, 0X08, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00,
  0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00,
  0X00, 0X00, 0X29, 0X28, 0X24, 0X22, 0X20, 0X1F, 0X1E, 0X1D,
  0X0E, 0X0C, 0X0A, 0X08, 0X06, 0X05, 0X04, 0X02, 0X00, 0XFF,
  0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00, 0X00,
  0X00, 0XFF, 0XFF, 0XFF, 0XFF, 0XFF, 0XFF, 0XFF, 0XFF, 0XFF,
  0XFF, 0XFF, 0XFF, 0XFF,
};


uint8_t GT9111_Send_Cfg(uint8_t mode)
{
  uint8_t buf[2];
  uint8_t i = 0;
  buf[0] = 0;
  buf[1] = mode;
  for (i = 0; i < sizeof(GT9111_CFG_TBL); i++)buf[0] += GT9111_CFG_TBL[i];
  buf[0] = (~buf[0]) + 1;
  GT911_WR_Reg(GT_CFGS_REG, (uint8_t*)GT9111_CFG_TBL, sizeof(GT9111_CFG_TBL));
  GT911_WR_Reg(GT_CHECK_REG, buf, 2);
  return 0;
}


void gt911_int_() {

  uint8_t buf[4];
  uint8_t CFG_TBL[184];
  
  pinMode(IIC_SDA, OUTPUT);
  pinMode(IIC_SCL, OUTPUT);
  pinMode(IIC_RST, OUTPUT);
//  pinMode(IIC_INT, OUTPUT);

  //  digitalWrite(IIC_RST, HIGH);
  //  digitalWrite(IIC_INT, HIGH);
  //  delay(50);
  //  digitalWrite(IIC_RST, LOW);
  //  digitalWrite(IIC_INT, LOW);
  //  delay(10);
  //  digitalWrite(IIC_INT, HIGH);
  //  delay(1);
  //  digitalWrite(IIC_RST, HIGH);
  //  delay(50);
  //  pinMode(IIC_INT, INPUT);
  //
  //  digitalWrite(IIC_INT, HIGH);

  delay(50);
  digitalWrite(IIC_RST, LOW);
//  digitalWrite(IIC_INT, LOW);
  delay(10);
  digitalWrite(IIC_RST, HIGH);
  delay(50);
 // pinMode(IIC_INT, INPUT);

  GT911_RD_Reg(0X8140, (uint8_t *)&buf, 4);
  Serial.printf("TouchPad_ID:%d,%d,%d\r\n", buf[0], buf[1], buf[2], buf[3]);
  buf[0] = 0x02;

  GT911_WR_Reg(GT_CTRL_REG, buf, 1);
  GT911_RD_Reg(GT_CFGS_REG, buf, 1);
  Serial.printf("Default Ver:0x%X\r\n", buf[0]);
  if (buf[0] < 0X60)
  {
    Serial.printf("Default Ver:0x%X\r\n", buf[0]);
    GT911_Send_Cfg(1);
  }
  
  GT911_RD_Reg(GT_CFGS_REG, (uint8_t *)&CFG_TBL[0], 184);
  for (uint8_t i = 0; i < sizeof(GT9111_CFG_TBL); i++)
  {

    Serial.printf("0x%02X  ", CFG_TBL[i]);
    if ((i + 1) % 10 == 0)
      Serial.printf("\r\n");
  } 
  delay( 10 );
  buf[0] = 0x00;
  GT911_WR_Reg(GT_CTRL_REG, buf, 1);
}

uint8_t GT9147_Scan(uint8_t mode)
{
  uint8_t buf[41];
   GT911_RD_Reg(GT911_READ_XY_REG, buf, 1); 
   Serial.printf("GT911_READ_XY_REG:%d\r\n", buf[0]);
   return 0;
}

void setup()
{ 
  Serial.begin( 115200 ); /*初始化串口*/
  Serial.println("Starting ESP32-3248S035 IP Display...");

  // TEMP/DEBUG: print why the chip actually reset. A "hang" that's really a
  // crash/watchdog-triggered reboot loop looks identical to the user (screen
  // just seems stuck/restarts), but this tells us which it was.
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  Serial.println("Reset reason: POWERON (normal power-on)"); break;
    case ESP_RST_SW:       Serial.println("Reset reason: SW (esp_restart() called)"); break;
    case ESP_RST_PANIC:    Serial.println("Reset reason: PANIC - crashed last boot!"); break;
    case ESP_RST_INT_WDT:  Serial.println("Reset reason: INT_WDT - interrupt watchdog!"); break;
    case ESP_RST_TASK_WDT: Serial.println("Reset reason: TASK_WDT - code hung last boot!"); break;
    case ESP_RST_WDT:      Serial.println("Reset reason: WDT - other watchdog reset!"); break;
    case ESP_RST_BROWNOUT: Serial.println("Reset reason: BROWNOUT - power dipped!"); break;
    default:                Serial.printf("Reset reason: %d (see esp_reset_reason_t)\r\n", (int)esp_reset_reason()); break;
  }

  // Initialize GT911 touch
  gt911_int_();
  
  // Initialize LVGL
    lv_init();

  #if LV_USE_LOG != 0
      lv_log_register_print_cb( my_print ); /* 用于调试的注册打印功能 */
  #endif
  
  // Initialize TFT
  tft.begin();          /*初始化*/
  tft.setRotation(1);    /* 旋转 - Landscape mode */
  tft.fillScreen(TFT_BLACK);

  // Initialize LVGL display buffer
  lv_disp_draw_buf_init( &draw_buf, buf, NULL, screenWidth * 10 );
  
  // Initialize display driver
  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init( &disp_drv );
  disp_drv.hor_res = screenWidth;
  disp_drv.ver_res = screenHeight;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register( &disp_drv );

  // Initialize input device driver
  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init( &indev_drv );
    indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touchpad_read;
  lv_indev_drv_register( &indev_drv );

  // Calibrate touch (2-point, on-device) before building the real UI, so
  // every interactive element benefits from it.
  Serial.println("Running touch calibration...");
  runTouchCalibration();

  // Create the chat UI first (show immediately)
  Serial.println("Creating chat screen...");
  createChatScreen();

  // Force initial display refresh
  lv_refr_now(NULL);
  Serial.println("Initial display created");

  // TF/micro-SD card (multi-network WiFi storage - see the comment above
  // loadSavedWifi()). Non-fatal if there's no card inserted or it fails to
  // init: g_sdReady just stays false and everything below transparently
  // falls back to the single NVS-saved network, same as before this existed.
  g_sdReady = SD.begin(TF_CS);
  Serial.println(g_sdReady ? "SD card ready" : "No SD card / init failed - using NVS-only WiFi storage");

  // WiFi: try every saved network (SD list, then the single NVS entry -
  // see connectToAnySavedWifi()) first; if none are saved (first boot) or
  // none work, fall back to secrets.h once as a legacy convenience, then
  // finally show the scan-and-tap setup screen (see runWifiSetupFlow())
  // rather than requiring a reflash to change networks.
  lv_label_set_text(chat_response_label, "Connecting to saved WiFi...");
  lv_refr_now(NULL);
  bool wifiConnected = connectToAnySavedWifi(chat_response_label);
  if (!wifiConnected && strlen(WIFI_SSID) > 0) {
    lv_label_set_text(chat_response_label, "Connecting to WiFi...");
    lv_refr_now(NULL);
    wifiConnected = tryConnectWifi(WIFI_SSID, WIFI_PASSWORD, chat_response_label);
    if (wifiConnected) persistWifiCredential(WIFI_SSID, WIFI_PASSWORD); // adopt it going forward
  }
  if (!wifiConnected) {
    Serial.println("No working saved WiFi - starting on-device setup...");
    wifiConnected = runWifiSetupFlow();
    createChatScreen(); // the setup screens overwrote the chat UI - rebuild it
    lv_refr_now(NULL);
  }

  // Configure time only if WiFi is connected
  if (wifiConnected) {
    Serial.println("Configuring time...");
    configTime(GMT_OFFSET, DAYLIGHT_OFFSET, NTP_SERVER);
    lv_label_set_text(chat_response_label, "Connected! Ask me anything.");
  } else {
    Serial.println("No WiFi - running in offline mode");
    lv_label_set_text(chat_response_label, "No WiFi - couldn't reach Gemini. Ask me anything once connected.");
  }
  lv_refr_now(NULL);

  Serial.println( "Setup done - IP Display should be running" );
}

void loop()
{
  // NOTE: don't call GT911_Scan() directly here - lv_timer_handler() already
  // polls touch via the registered indev (my_touchpad_read -> readRawTouch
  // -> GT911_Scan). Calling it again here was pure waste: GT911_Scan()
  // blocks for ~10ms whenever there's no touch, so doing it twice per loop
  // needlessly halved the effective poll rate and made the keyboard feel
  // laggy.
  lv_timer_handler(); /* 让GUI完成它的工作 */

  // changeWifiFromChatBtnEventCb() (the chat screen's gear button) only
  // sets this flag rather than calling runWifiSetupFlow() directly, because
  // that button's own click handler runs SYNCHRONOUSLY INSIDE the
  // lv_timer_handler() call above (LVGL dispatches the click as part of
  // indev processing). runWifiSetupFlow() has its own internal loop that
  // also calls lv_timer_handler() - calling it while already inside a
  // lv_timer_handler() call is a reentrant call LVGL isn't designed for,
  // and corrupts its indev press/release tracking. That's almost certainly
  // why every WiFi-setup-screen button stayed unresponsive no matter how
  // the screen was built (list, picker, scrollable or not) - it was never
  // about the widgets, it was about calling into LVGL from the wrong place.
  // Handling the request here instead means runWifiSetupFlow() only ever
  // runs from this top-level, non-nested call site.
  if (g_wifiSetupRequested) {
    g_wifiSetupRequested = false;
    bool connected = runWifiSetupFlow();
    createChatScreen(); // the setup screens overwrote the chat UI - rebuild it
    lv_refr_now(NULL);
    lv_label_set_text(chat_response_label,
                       connected ? "WiFi updated! Ask me anything."
                                 : "Still offline - ask me anything once connected.");
  }

  delay( 10 );
}
/*
  void touch_calibrate()//屏幕校准
  {
  uint16_t calData[5];
  uint8_t calDataOK = 0;

  //校准
  tft.fillScreen(TFT_BLACK);
  tft.setCursor(20, 0)
  tft.setTextFont(2);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  tft.println("按指示触摸角落");

  tft.setTextFont(1);
  tft.println();

  //tft.calibrateTouch(calData, TFT_MAGENTA, TFT_BLACK, 15);

  Serial.println(); Serial.println();
  Serial.println("//在setup()中使用此校准代码:");
  Serial.print("uint16_t calData[5] = ");
  Serial.print("{ ");

  for (uint8_t i = 0; i < 5; i++)
  {
    Serial.print(calData[i]);
    if (i < 4) Serial.print(", ");
  }

  Serial.println(" };");
  Serial.print("  tft.setTouch(calData);");
  Serial.println(); Serial.println();

  tft.fillScreen(TFT_BLACK);

  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.println("XZ OK!");
  tft.println("Calibration code sent to Serial port.");

  }
*/