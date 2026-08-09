#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include <lvgl.h>
#include <TFT_eSPI.h>

#include <demos/lv_demos.h>

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


//reg:起始寄存器地址
//buf:数据缓缓存区
//len:写数据长度
//返回值:0,成功;1,失败.
uint8_t GT911_WR_Reg(uint16_t reg, uint8_t *buf, uint8_t len)
{
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
  return ret;
}

//reg:起始寄存器地址
//buf:数据缓缓存区
//len:读数据长度
void GT911_RD_Reg(uint16_t reg, uint8_t *buf, uint8_t len)
{
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
      delay(10);
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
  // strip near the top (and likely the right) of the touch-sensitive area,
  // so points too close to those edges never register a touch at all.
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

// WiFi Connection Function
void connectToWiFi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);
  
  // WiFi status codes for debugging
  const char* wifiStatusText[] = {
    "IDLE_STATUS", "NO_SSID_AVAIL", "SCAN_COMPLETED", "CONNECTED", 
    "CONNECT_FAILED", "CONNECTION_LOST", "DISCONNECTED"
  };
  
  // Disconnect and reset WiFi
  WiFi.disconnect(true);
  delay(1000);
  
  // Set WiFi mode to Station
  WiFi.mode(WIFI_STA);
  delay(100);
  
  // Scan for available networks first
  Serial.println("Scanning for networks...");
  int n = WiFi.scanNetworks();
  Serial.print("Found ");
  Serial.print(n);
  Serial.println(" networks:");
  
  bool networkFound = false;
  for (int i = 0; i < n; i++) {
    Serial.print(i + 1);
    Serial.print(": ");
    Serial.print(WiFi.SSID(i));
    Serial.print(" (");
    Serial.print(WiFi.RSSI(i));
    Serial.print(" dBm) ");
    Serial.println((WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "Open" : "Encrypted");
    
    if (WiFi.SSID(i) == WIFI_SSID) {
      networkFound = true;
      Serial.print("*** Target network found! ***");
    }
  }
  
  if (!networkFound) {
    Serial.println("ERROR: Target network 'wifilab' not found in scan!");
    Serial.println("Please check the SSID name.");
    return;
  }
  
  // Begin connection
  Serial.println("Starting connection...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  int attempts = 0;
  const int maxAttempts = 20; // 20 seconds timeout
  
  while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts) {
    delay(1000);
    attempts++;
    int status = WiFi.status();
    Serial.print("Attempt ");
    Serial.print(attempts);
    Serial.print("/");
    Serial.print(maxAttempts);
    Serial.print(" - WiFi status: ");
    Serial.print(status);
    if (status >= 0 && status <= 6) {
      Serial.print(" (");
      Serial.print(wifiStatusText[status]);
      Serial.print(")");
    }
    Serial.println();
    
    // Print signal strength if available
    if (status == WL_CONNECTED || status == WL_DISCONNECTED) {
      Serial.print("Signal strength: ");
      Serial.print(WiFi.RSSI());
      Serial.println(" dBm");
    }
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    Serial.print("Signal strength: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
    } else {
    Serial.println("WiFi connection failed!");
    Serial.print("Final status: ");
    Serial.println(WiFi.status());
    Serial.println("Possible issues:");
    Serial.println("1. Wrong SSID or password");
    Serial.println("2. WiFi network not available");
    Serial.println("3. Signal too weak");
    Serial.println("4. Network security issues");
  }
}

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

// Two layouts: "normal" (reply box + input row + Send button) and "typing"
// (reply box hidden, input box moved to the very top and widened to fill
// the space Send normally occupies - Send is hidden since the keyboard's
// own Enter/checkmark key submits - and the keyboard fills essentially the
// rest of the screen).
void setChatTypingMode(bool typing) {
  if (typing) {
    lv_obj_add_flag(chat_response_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(chat_send_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(chat_input_ta, 460, 40);
    lv_obj_align(chat_input_ta, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_set_size(chat_kb, 460, 260);
    lv_obj_align(chat_kb, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_clear_flag(chat_kb, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_clear_flag(chat_response_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(chat_send_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(chat_input_ta, 340, 40);
    lv_obj_align(chat_input_ta, LV_ALIGN_TOP_LEFT, 10, 270);
    lv_obj_align(chat_send_btn, LV_ALIGN_TOP_RIGHT, -10, 270);
    lv_obj_add_flag(chat_kb, LV_OBJ_FLAG_HIDDEN);
  }
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
void chatTextareaEventCb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_FOCUSED) {
    setChatTypingMode(true);
  } else if (code == LV_EVENT_DEFOCUSED) {
    setChatTypingMode(false);
  }
}

void chatKeyboardEventCb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY) { // Enter/checkmark key
    sendChatMessage(e);
  } else if (code == LV_EVENT_CANCEL) { // hide/close key
    setChatTypingMode(false);
  }
}

// Builds the entire (only) screen: reply area on top, input box + Send
// button below it, and an lv_keyboard that pops up when the input box is
// focused. Called once from setup().
void createChatScreen() {
  lv_obj_clean(lv_scr_act());
  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x0f172a), 0);

  // Reply area - fills essentially the whole screen (input row is pinned to
  // the bottom below it). Hidden while typing (see setChatTypingMode) so
  // the input box + keyboard can take over instead.
  chat_response_box = lv_obj_create(lv_scr_act());
  lv_obj_set_size(chat_response_box, 460, 255);
  lv_obj_align(chat_response_box, LV_ALIGN_TOP_MID, 0, 5);
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
  lv_obj_add_event_cb(chat_send_btn, sendChatMessage, LV_EVENT_CLICKED, NULL);
  lv_obj_set_style_radius(chat_send_btn, 16, 0);
  lv_obj_t *send_label = lv_label_create(chat_send_btn);
  lv_label_set_text(send_label, "Send");
  lv_obj_center(send_label);

  // Keyboard, hidden until the textarea is focused. setChatTypingMode()
  // resizes/repositions it (and the input row) each time it's shown/hidden.
  // Styled as a rounded bordered "card" to match the input box.
  chat_kb = lv_keyboard_create(lv_scr_act());
  lv_keyboard_set_textarea(chat_kb, chat_input_ta);
  lv_obj_add_event_cb(chat_kb, chatKeyboardEventCb, LV_EVENT_ALL, NULL);
  lv_obj_set_style_radius(chat_kb, 16, 0);
  lv_obj_set_style_border_width(chat_kb, 2, 0);
  lv_obj_set_style_border_color(chat_kb, lv_color_hex(0x22c55e), 0);
  lv_obj_set_style_bg_color(chat_kb, lv_color_hex(0x0f172a), 0);
  lv_obj_set_style_pad_all(chat_kb, 8, 0);
  lv_obj_add_flag(chat_kb, LV_OBJ_FLAG_HIDDEN);
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
  tft.fillScreen(TFT_RED);
  delay(500);
  tft.fillScreen(TFT_GREEN);
  delay(500);
  tft.fillScreen(TFT_BLUE);
  delay(500);
  tft.fillScreen(TFT_BLACK);
  tft.drawRect(0, 0, 480, 320, TFT_RED);
  delay(500);
  
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
  
  // connectToWiFi() blocks for several seconds (network scan + connect
  // attempts) with no screen updates of its own, which can look like the
  // device hung right after calibration. Show status on the chat screen
  // (already visible at this point) so it's clear something's happening.
  lv_label_set_text(chat_response_label, "Connecting to WiFi...");
  lv_refr_now(NULL);
  Serial.println("Connecting to WiFi...");
  connectToWiFi();

  // Configure time only if WiFi is connected
  if (WiFi.status() == WL_CONNECTED) {
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