/*
 * This file is part of the libopeninv project.
 *
 * Copyright (C) 2016 Nail Güzel
 * Johannes Huebner <dev@johanneshuebner.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdint.h>
#include "hwdefs.h"
#include "my_string.h"
#include "my_math.h"
#include "printf.h"
#include <libopencm3/stm32/can.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/flash.h>
#include <libopencm3/stm32/crc.h>
#include <libopencm3/stm32/rtc.h>
#include <libopencm3/stm32/desig.h>
#include <libopencm3/cm3/common.h>
#include <libopencm3/cm3/nvic.h>
#include "stm32_can.h"

#define MAX_INTERFACES        2
#define IDS_PER_BANK          4
#define SDO_WRITE             0x40
#define SDO_READ              0x22
#define SDO_ABORT             0x80
#define SDO_WRITE_REPLY       0x23
#define SDO_READ_REPLY        0x43
#define SDO_ERR_INVIDX        0x06020000
#define SDO_ERR_RANGE         0x06090030
#define SENDMAP_ADDRESS(b)    b
#define RECVMAP_ADDRESS(b)    (b + sizeof(canSendMap))
#define CRC_ADDRESS(b)        (b + sizeof(canSendMap) + sizeof(canRecvMap))
#define SENDMAP_WORDS         (sizeof(canSendMap) / (sizeof(uint32_t)))
#define RECVMAP_WORDS         (sizeof(canRecvMap) / (sizeof(uint32_t)))
#define CANID_UNSET           0xffffffff
#define NUMBITS_LASTMARKER    -1
#define forEachCanMap(c,m) for (CANIDMAP *c = m; (c - m) < MAX_MESSAGES && c->canId < CANID_UNSET; c++)
#define forEachPosMap(c,m) for (CANPOS *c = m->items; (c - m->items) < MAX_ITEMS_PER_MESSAGE && c->numBits > 0; c++)

#if (2 *((MAX_ITEMS_PER_MESSAGE * 10 + 4) * MAX_MESSAGES + 2) + 4) > CAN_BLKSIZE
#error CANMAP will not fit in one flash page
#endif

struct CAN_SDO
{
   uint8_t cmd;
   uint16_t index;
   uint8_t subIndex;
   uint32_t data;
} __attribute__((packed));

struct CANSPEED
{
   uint32_t ts1;
   uint32_t ts2;
   uint32_t prescaler;
};

Can* Can::interfaces[MAX_INTERFACES];
volatile bool Can::isSaving = false;

static void DummyCallback(uint32_t i, uint32_t* d) { i=i; d=d; }
static const CANSPEED canSpeed[Can::BaudLast] =
{
   { CAN_BTR_TS1_13TQ, CAN_BTR_TS2_2TQ, 21 }, //125kbps
   { CAN_BTR_TS1_11TQ, CAN_BTR_TS2_2TQ, 12 }, //250kbps
   { CAN_BTR_TS1_11TQ, CAN_BTR_TS2_2TQ, 6 }, //500kbps
   { CAN_BTR_TS1_11TQ, CAN_BTR_TS2_2TQ, 3 }, //1000kbps
};

/** \brief Add periodic CAN message
 *
 * \param param Parameter index of parameter to be sent
 * \param canId CAN identifier of generated message
 * \param offset bit offset within the 64 message bits
 * \param length number of bits
 * \param gain Fixed point gain to be multiplied before sending
 * \return success: number of active messages
 * Fault:
 * - CAN_ERR_INVALID_ID ID was > 0x1fffffff
 * - CAN_ERR_INVALID_OFS Offset > 63
 * - CAN_ERR_INVALID_LEN Length > 32
 * - CAN_ERR_MAXMESSAGES Already 10 send messages defined
 * - CAN_ERR_MAXITEMS Already 8 items in message
 */
int Can::AddSend(Param::PARAM_NUM param, int canId, int offsetBits, int length, float gain, int16_t offset)
{
   return Add(canSendMap, param, canId, offsetBits, length, gain, offset);
}

int Can::AddSend(Param::PARAM_NUM param, int canId, int offsetBits, int length, float gain)
{
   return Add(canSendMap, param, canId, offsetBits, length, gain, 0);
}

/** \brief Map data from CAN bus to parameter
 *
 * \param param Parameter index of parameter to be received
 * \param canId CAN identifier of consumed message
 * \param offset bit offset within the 64 message bits
 * \param length number of bits
 * \param gain Fixed point gain to be multiplied after receiving
 * \return success: number of active messages
 * Fault:
 * - CAN_ERR_INVALID_ID ID was > 0x1fffffff
 * - CAN_ERR_INVALID_OFS Offset > 63
 * - CAN_ERR_INVALID_LEN Length > 32
 * - CAN_ERR_MAXMESSAGES Already 10 receive messages defined
 * - CAN_ERR_MAXITEMS Already 8 items in message
 */
int Can::AddRecv(Param::PARAM_NUM param, int canId, int offsetBits, int length, float gain, int16_t offset)
{
   int res = Add(canRecvMap, param, canId, offsetBits, length, gain, offset);
   ConfigureFilters();
   return res;
}

int Can::AddRecv(Param::PARAM_NUM param, int canId, int offsetBits, int length, float gain)
{
   return Can::AddRecv(param, canId, offsetBits, length, gain, 0);
}

/** \brief Set function to be called for user handled CAN messages
 *
 * \param recv Function pointer to void func(uint32_t, uint32_t[2]) - ID, Data
 */
void Can::SetReceiveCallback(void (*recv)(uint32_t, uint32_t*))
{
   recvCallback = recv;
}

/** \brief Add CAN Id to user message list
 * \post Receive callback will be called when a message with this Id id received
 * \param canId CAN identifier of message to be user handled
 * \return true: success, false: already 10 messages registered
 *
 */
bool Can::RegisterUserMessage(int canId)
{
   if (nextUserMessageIndex < MAX_USER_MESSAGES)
   {
      userIds[nextUserMessageIndex] = canId;
      nextUserMessageIndex++;
      ConfigureFilters();
      return true;
   }
   return false;
}

/** \brief Find first occurence of parameter in CAN map and output its mapping info
 *
 * \param[in] param Index of parameter to be looked up
 * \param[out] canId CAN identifier that the parameter is mapped to
 * \param[out] offset bit offset that the parameter is mapped to
 * \param[out] length number of bits that the parameter is mapped to
 * \param[out] gain Parameter gain
 * \param[out] rx true: Parameter is received via CAN, false: sent via CAN
 * \return true: parameter is mapped, false: not mapped
 */
bool Can::FindMap(Param::PARAM_NUM param, int& canId, int& offset, int& length, float& gain, bool& rx)
{
   rx = false;
   bool done = false;

   for (CANIDMAP *map = canSendMap; !done; map = canRecvMap)
   {
      forEachCanMap(curMap, map)
      {
         forEachPosMap(curPos, curMap)
         {
            if (curPos->mapParam == param)
            {
               canId = curMap->canId;
               offset = curPos->offsetBits;
               length = curPos->numBits;
               gain = curPos->gain;
               return true;
            }
         }
      }
      done = rx;
      rx = true;
   }
   return false;
}

/** \brief Save CAN mapping to flash
 *  \pre the flash page/sector needs to be erased prior to calling this function
 */
void Can::Save()
{
   uint32_t crc;
   uint32_t baseAddress = GetFlashAddress();

   isSaving = true;

   crc_reset();

   ReplaceParamEnumByUid(canSendMap);
   ReplaceParamEnumByUid(canRecvMap);

   SaveToFlash(baseAddress, (uint32_t *)canSendMap, SENDMAP_WORDS);
   crc = SaveToFlash(RECVMAP_ADDRESS(baseAddress), (uint32_t *)canRecvMap, RECVMAP_WORDS);
   SaveToFlash(CRC_ADDRESS(baseAddress), &crc, 1);

   ReplaceParamUidByEnum(canSendMap);
   ReplaceParamUidByEnum(canRecvMap);

   isSaving = false;
}

/** \brief Send all defined messages
 */
void Can::SendAll()
{
   forEachCanMap(curMap, canSendMap)
   {
      uint32_t data[2] = { 0 }; //Had an issue with uint64_t, otherwise would have used that

      forEachPosMap(curPos, curMap)
      {
         if (isSaving) return; //Only send mapped messages when not currently saving to flash

         float fval = Param::GetFloat((Param::PARAM_NUM)curPos->mapParam);
         fval *= curPos->gain;
         fval += curPos->offset;
         uint32_t val = fval;
         val &= ((1 << curPos->numBits) - 1);

         if (curPos->offsetBits > 31)
         {
            data[1] |= val << (curPos->offsetBits - 32);
         }
         else
         {
            data[0] |= val << curPos->offsetBits;
         }
      }

      Send(curMap->canId, data);
   }
}

/** \brief Clear all defined messages
 */
void Can::Clear()
{
   ClearMap(canSendMap);
   ClearMap(canRecvMap);
   ConfigureFilters();
}

/** \brief Remove all occurences of given parameter from CAN map
 *
 * \param param Parameter index to be removed
 * \return int number of removed items
 *
 */
int Can::Remove(Param::PARAM_NUM param)
{
   int removed = RemoveFromMap(canSendMap, param);
   removed += RemoveFromMap(canRecvMap, param);

   return removed;
}

/** \brief Init can hardware with given baud rate
 * Initializes the following sub systems:
 * - CAN hardware itself
 * - Appropriate GPIO pins
 * - Enables appropriate interrupts in NVIC
 *
 * \param baseAddr base address of CAN peripheral, CAN1 or CAN2
 * \param baudrate enum baudrates
 * \param remap use remapped IO pins
 * \return void
 *
 */
Can::Can(uint32_t baseAddr, enum baudrates baudrate, bool remap)
   : lastRxTimestamp(0), sendCnt(0), recvCallback(DummyCallback), nextUserMessageIndex(0), canDev(baseAddr)
{
   Clear();
   LoadFromFlash();

   switch (baseAddr)
   {
      case CAN1:
         // Configure CAN pins
         gpio_mode_setup(GPIOB, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO8 | GPIO9);
         gpio_set_af(GPIOB, GPIO_AF9, GPIO8 | GPIO9);

         //CAN1 RX and TX IRQs
         nvic_enable_irq(NVIC_CAN1_RX0_IRQ); //CAN RX
         nvic_set_priority(NVIC_CAN1_RX0_IRQ, 0xf << 4); //lowest priority
         nvic_enable_irq(NVIC_CAN1_RX1_IRQ); //CAN RX
         nvic_set_priority(NVIC_CAN1_RX1_IRQ, 0xf << 4); //lowest priority
         nvic_enable_irq(NVIC_CAN1_TX_IRQ); //CAN TX
         nvic_set_priority(NVIC_CAN1_TX_IRQ, 0xf << 4); //lowest priority
         interfaces[0] = this;
         break;
      case CAN2:
         // Configure CAN pins
         gpio_mode_setup(GPIOB, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO12 | GPIO13);
         gpio_set_af(GPIOB, GPIO_AF9, GPIO12 | GPIO13);

         //CAN2 RX and TX IRQs
         nvic_enable_irq(NVIC_CAN2_RX0_IRQ); //CAN RX
         nvic_set_priority(NVIC_CAN2_RX0_IRQ, 0xf << 4); //lowest priority
         nvic_enable_irq(NVIC_CAN2_RX1_IRQ); //CAN RX
         nvic_set_priority(NVIC_CAN2_RX1_IRQ, 0xf << 4); //lowest priority
         nvic_enable_irq(NVIC_CAN2_TX_IRQ); //CAN RX
         nvic_set_priority(NVIC_CAN2_TX_IRQ, 0xf << 4); //lowest priority
         interfaces[1] = this;
         break;
   }

   nodeId = 1;
	// Reset CAN
	can_reset(canDev);

	SetBaudrate(baudrate);
   ConfigureFilters();
	// Enable CAN RX interrupts.
	can_enable_irq(canDev, CAN_IER_FMPIE0);
	can_enable_irq(canDev, CAN_IER_FMPIE1);
}

/** \brief Set baud rate to given value
 *
 * \param baudrate enum baudrates
 * \return void
 *
 */
void Can::SetBaudrate(enum baudrates baudrate)
{
	// CAN cell init.
	 // Setting the bitrate to 250KBit. APB1 = 36MHz,
	 // prescaler = 9 -> 4MHz time quanta frequency.
	 // 1tq sync + 9tq bit segment1 (TS1) + 6tq bit segment2 (TS2) =
	 // 16time quanto per bit period, therefor 4MHz/16 = 250kHz
	 //
	can_init(canDev,
		     false,          // TTCM: Time triggered comm mode?
		     true,           // ABOM: Automatic bus-off management?
		     false,          // AWUM: Automatic wakeup mode?
		     false,          // NART: No automatic retransmission?
		     false,          // RFLM: Receive FIFO locked mode?
		     false,          // TXFP: Transmit FIFO priority?
		     CAN_BTR_SJW_1TQ,
		     canSpeed[baudrate].ts1,
		     canSpeed[baudrate].ts2,
		     canSpeed[baudrate].prescaler,				// BRP+1: Baud rate prescaler
		     false,
		     false);
}

/** \brief Get RTC time when last message was received
 *
 * \return uint32_t RTC time
 *
 */
uint32_t Can::GetLastRxTimestamp()
{
   return lastRxTimestamp;
}

/** \brief Send a user defined CAN message
 *
 * \param canId uint32_t
 * \param data[2] uint32_t
 * \param len message length
 * \return void
 *
 */
void Can::Send(uint32_t canId, uint32_t data[2], uint8_t len)
{
   can_disable_irq(canDev, CAN_IER_TMEIE);

   if (can_transmit(canDev, canId, canId > 0x7FF, false, len, (uint8_t*)data) < 0 && sendCnt < SENDBUFFER_LEN)
   {
      // enqueue in send buffer if all TX mailboxes are full
      sendBuffer[sendCnt].id = canId;
      sendBuffer[sendCnt].len = len;
      sendBuffer[sendCnt].data[0] = data[0];
      sendBuffer[sendCnt].data[1] = data[1];
      sendCnt++;
   }

   if (sendCnt > 0)
   {
      can_enable_irq(canDev, CAN_IER_TMEIE);
   }
}

void Can::IterateCanMap(void (*callback)(Param::PARAM_NUM, int, int, int, float, bool))
{
   bool done = false, rx = false;

   for (CANIDMAP *map = canSendMap; !done; map = canRecvMap)
   {
      forEachCanMap(curMap, map)
      {
         forEachPosMap(curPos, curMap)
         {
            callback((Param::PARAM_NUM)curPos->mapParam, curMap->canId, curPos->offsetBits, curPos->numBits, curPos->gain, rx);
         }
      }
      done = rx;
      rx = true;
   }
}

Can* Can::GetInterface(int index)
{
   if (index < MAX_INTERFACES)
   {
      return interfaces[index];
   }
   return 0;
}

void Can::HandleRx(int fifo)
{
   uint32_t id;
	bool ext, rtr;
	uint8_t length, fmi;
	uint32_t data[2];

   while (can_receive(canDev, fifo, true, &id, &ext, &rtr, &fmi, &length, (uint8_t*)data, NULL) > 0)
   {
      //printf("fifo: %d, id: %x, len: %d, data[0]: %x, data[1]: %x\r\n", fifo, id, length, data[0], data[1]);
      if (id == (0x600U + nodeId) && length == 8) //SDO request, nodeid=1
      {
         ProcessSDO(data);
      }
      else
      {
         if (isSaving) continue; //Only handle mapped messages when not currently saving to flash

         CANIDMAP *recvMap = FindById(canRecvMap, id);

         if (0 != recvMap)
         {
            forEachPosMap(curPos, recvMap)
            {
               s32fp val;

               if (curPos->offsetBits > 31)
               {
                  val = FP_FROMINT((data[1] >> (curPos->offsetBits - 32)) & ((1 << curPos->numBits) - 1));
               }
               else
               {
                  val = FP_FROMINT((data[0] >> curPos->offsetBits) & ((1 << curPos->numBits) - 1));
               }
               val+= curPos->offset;
               val*= curPos->gain;

               if (Param::IsParam((Param::PARAM_NUM)curPos->mapParam))
                  Param::Set((Param::PARAM_NUM)curPos->mapParam, val);
               else
                  Param::SetFixed((Param::PARAM_NUM)curPos->mapParam, val);
            }
            //lastRxTimestamp = rtc_get_counter_val();
         }
         else //Now it must be a user message, as filters block everything else
         {
            recvCallback(id, data);
         }
      }
   }
}

void Can::HandleTx()
{
   SENDBUFFER* b = sendBuffer; //alias

   while (sendCnt > 0 && can_transmit(canDev, b[sendCnt - 1].id, b[sendCnt - 1].id > 0x7FF, false, b[sendCnt - 1].len, (uint8_t*)b[sendCnt - 1].data) >= 0)
      sendCnt--;

   if (sendCnt == 0)
   {
      can_disable_irq(canDev, CAN_IER_TMEIE);
   }
}

void Can::SDOWrite(uint8_t remoteNodeId, uint16_t index, uint8_t subIndex, uint32_t data)
{
   uint32_t d[2];
   CAN_SDO *sdo = (CAN_SDO*)d;

   sdo->cmd = SDO_WRITE;
   sdo->index = index;
   sdo->subIndex = subIndex;
   sdo->data = data;

   Send(0x600 + remoteNodeId, d);
}

/****************** Private methods and ISRs ********************/

//http://www.byteme.org.uk/canopenparent/canopen/sdo-service-data-objects-canopen/
void Can::ProcessSDO(uint32_t data[2])
{
   CAN_SDO *sdo = (CAN_SDO*)data;
   if (sdo->index >= 0x2000 && sdo->index <= 0x2001 && sdo->subIndex < Param::PARAM_LAST)
   {
      Param::PARAM_NUM paramIdx = (Param::PARAM_NUM)sdo->subIndex;

      //SDO index 0x2001 will lookup the parameter by its unique ID
      if (sdo->index == 0x2001)
         paramIdx = Param::NumFromId(sdo->subIndex);

      if (sdo->cmd == SDO_WRITE)
      {
         if (Param::Set(paramIdx, sdo->data) == 0)
         {
            sdo->cmd = SDO_WRITE_REPLY;
         }
         else
         {
            sdo->cmd = SDO_ABORT;
            sdo->data = SDO_ERR_RANGE;
         }
      }
      else if (sdo->cmd == SDO_READ)
      {
         sdo->data = Param::Get(paramIdx);
         sdo->cmd = SDO_READ_REPLY;
      }
   }
   else if (sdo->index >= 0x3000 && sdo->index < 0x4800 && sdo->subIndex < Param::PARAM_LAST)
   {
      if (sdo->cmd == SDO_WRITE)
      {
         int result;
         int offset = sdo->data & 0xFF;
         int len = (sdo->data >> 8) & 0xFF;
         s32fp gain = sdo->data >> 16;

         if ((sdo->index & 0x4000) == 0x4000)
         {
            result = Can::AddRecv((Param::PARAM_NUM)sdo->subIndex, sdo->index & 0x7FF, offset, len, gain);
         }
         else
         {
            result = Can::AddSend((Param::PARAM_NUM)sdo->subIndex, sdo->index & 0x7FF, offset, len, gain);
         }

         if (result >= 0)
         {
            sdo->cmd = SDO_WRITE_REPLY;
         }
         else
         {
            sdo->cmd = SDO_ABORT;
            sdo->data = SDO_ERR_RANGE;
         }
      }
   }
   else
   {
      sdo->cmd = SDO_ABORT;
      sdo->data = SDO_ERR_INVIDX;
   }
   Can::Send(0x580 + nodeId, data);
}

void Can::SetFilterBank(int& idIndex, int& filterId, uint16_t* idList)
{
   can_filter_id_list_16bit_init(
         filterId,
         idList[0] << 5, //left align
         idList[1] << 5,
         idList[2] << 5,
         idList[3] << 5,
         filterId & 1,
         true);
   idIndex = 0;
   filterId++;
   idList[0] = idList[1] = idList[2] = idList[3] = 0;
}

void Can::ConfigureFilters()
{
   uint16_t idList[IDS_PER_BANK] = { 0, 0, 0, 0 };
   int idIndex = 1;
   int filterId = canDev == CAN1 ? 0 : ((CAN_FMR(CAN2) >> 8) & 0x3F);

   idList[0] = 0x600 + nodeId;

   for (int i = 0; i < nextUserMessageIndex; i++)
   {
      idList[idIndex] = userIds[i];
      idIndex++;

      if (idIndex == IDS_PER_BANK)
      {
         SetFilterBank(idIndex, filterId, idList);
      }
   }

   forEachCanMap(curMap, canRecvMap)
   {
      idList[idIndex] = curMap->canId;
      idIndex++;

      if (idIndex == IDS_PER_BANK)
      {
         SetFilterBank(idIndex, filterId, idList);
      }
   }
   //loop terminates before adding last set of filters
   if (idIndex > 0)
   {
      SetFilterBank(idIndex, filterId, idList);
   }
}

int Can::LoadFromFlash()
{
   uint32_t data = GetFlashAddress();
   uint32_t storedCrc = *(uint32_t*)CRC_ADDRESS(data);
   uint32_t crc;

   crc_reset();
   crc = crc_calculate_block((uint32_t*)data, SENDMAP_WORDS + RECVMAP_WORDS);

   if (storedCrc == crc)
   {
      memcpy32((int*)canSendMap, (int*)SENDMAP_ADDRESS(data), SENDMAP_WORDS);
      memcpy32((int*)canRecvMap, (int*)RECVMAP_ADDRESS(data), RECVMAP_WORDS);
      ReplaceParamUidByEnum(canSendMap);
      ReplaceParamUidByEnum(canRecvMap);
      return 1;
   }
   return 0;
}

int Can::RemoveFromMap(CANIDMAP *canMap, Param::PARAM_NUM param)
{
   CANIDMAP copyMap[MAX_MESSAGES];

   ClearMap(copyMap);
   int removed = CopyIdMapExcept(canMap, copyMap, param);
   ClearMap(canMap);
   CopyIdMapExcept(copyMap, canMap, param);

   return removed;
}

int Can::Add(CANIDMAP *canMap, Param::PARAM_NUM param, int canId, int offsetBits, int length, float gain, int16_t offset)
{
   if (canId > 0x1fffffff) return CAN_ERR_INVALID_ID;
   if (offset > 63) return CAN_ERR_INVALID_OFS;
   if (length > 32) return CAN_ERR_INVALID_LEN;

   CANIDMAP *existingMap = FindById(canMap, canId);

   if (0 == existingMap)
   {
      existingMap = FindById(canMap, CANID_UNSET);
      if (0 == existingMap)
         return CAN_ERR_MAXMESSAGES;

      existingMap->canId = canId;
   }

   CANPOS* freeItem = existingMap->items;

   for (; freeItem->numBits > 0; freeItem++);

   if (freeItem->numBits == NUMBITS_LASTMARKER)
      return CAN_ERR_MAXITEMS;

   freeItem->mapParam = param;
   freeItem->gain = gain;
   freeItem->offset = offset;
   freeItem->offsetBits = offsetBits;
   freeItem->numBits = length;

   int count = 0;

   forEachCanMap(curMap, canMap)
      count++;

   return count;
}

void Can::ClearMap(CANIDMAP *canMap)
{
   for (int i = 0; i < MAX_MESSAGES; i++)
   {
      canMap[i].canId = CANID_UNSET;

      for (int j = 0; j < MAX_ITEMS_PER_MESSAGE; j++)
      {
         canMap[i].items[j].numBits = 0;
      }
   }
}

Can::CANIDMAP* Can::FindById(CANIDMAP *canMap, uint32_t canId)
{
   for (int i = 0; i < MAX_MESSAGES; i++)
   {
      if (canMap[i].canId == canId)
         return &canMap[i];
   }
   return 0;
}

uint32_t Can::SaveToFlash(uint32_t baseAddress, uint32_t* data, int len)
{
   uint32_t crc = 0;

   for (int idx = 0; idx < len; idx++)
   {
      crc = crc_calculate(*data);
      flash_program_word(baseAddress + idx * sizeof(uint32_t), *data);
      data++;
   }

   return crc;
}

int Can::CopyIdMapExcept(CANIDMAP *source, CANIDMAP *dest, Param::PARAM_NUM param)
{
   int i = 0, removed = 0;

   forEachCanMap(curMap, source)
   {
      bool discardId = true;
      int j = 0;

      forEachPosMap(curPos, curMap)
      {
         if (curPos->mapParam != param)
         {
            discardId = false;
            dest[i].items[j] = *curPos;
            j++;
         }
         else
         {
            removed++;
         }
      }

      if (!discardId)
      {
         dest[i].canId = curMap->canId;
         i++;
      }
   }

   return removed;
}

void Can::ReplaceParamEnumByUid(CANIDMAP *canMap)
{
   forEachCanMap(curMap, canMap)
   {
      forEachPosMap(curPos, curMap)
      {
         const Param::Attributes* attr = Param::GetAttrib((Param::PARAM_NUM)curPos->mapParam);
         curPos->mapParam = (uint16_t)attr->id;
      }
   }
}

void Can::ReplaceParamUidByEnum(CANIDMAP *canMap)
{
   forEachCanMap(curMap, canMap)
   {
      forEachPosMap(curPos, curMap)
      {
         Param::PARAM_NUM param = Param::NumFromId(curPos->mapParam);
         curPos->mapParam = param;
      }
   }
}

uint32_t Can::GetFlashAddress()
{
   switch (this->canDev)
   {
      case CAN1:
         return FLASH_CONF_BASE + CAN1_BLKOFFSET;
      case CAN2:
         return FLASH_CONF_BASE + CAN2_BLKOFFSET;
   }
   return 0;
}

/* Interrupt service routines */
extern "C" void can1_rx0_isr(void)
{
   Can::GetInterface(0)->HandleRx(0);
}

extern "C" void can1_rx1_isr()
{
   Can::GetInterface(0)->HandleRx(1);
}

extern "C" void can1_tx_isr()
{
   Can::GetInterface(0)->HandleTx();
}

extern "C" void can2_rx0_isr()
{
   Can::GetInterface(1)->HandleRx(0);
}

extern "C" void can2_rx1_isr()
{
   Can::GetInterface(1)->HandleRx(1);
}

extern "C" void can2_tx_isr()
{
   Can::GetInterface(1)->HandleTx();
}
