#include "board.h"
#include "gateway_journal.h"
#include "stm32g0xx_hal.h"
#include <string.h>

extern uint8_t __registry_page0[],__registry_page1[];
_Static_assert(BOARD_REGISTRY_CAPACITY==OSG_JOURNAL_CAPACITY,"Journal payload capacity");
static const uint8_t *page(void *context,unsigned index) {
    (void)context; if(index>1) return NULL;
    return index ? __registry_page1 : __registry_page0;
}
static bool page_address(unsigned index,uint32_t *address) {
    if(index>1 || FLASH_SIZE!=128U*1024U) return false;
    *address=(uint32_t)(uintptr_t)page(NULL,index);
    return *address==(index ? 0x0801F800U : 0x0801F000U);
}
static bool erase_page(void *context,unsigned index) {
    (void)context; uint32_t address;
    if(!page_address(index,&address)) return false;
    /* Page indexes are relative to the active bank configuration, including
     * dual-bank devices. Never derive the final storage pages from a larger
     * STM32G0B1 device's family-wide maximum capacity. */
    FLASH_EraseInitTypeDef erase={.TypeErase=FLASH_TYPEERASE_PAGES,.Banks=FLASH_BANK_1,
        .Page=(address-FLASH_BASE)%FLASH_BANK_SIZE/FLASH_PAGE_SIZE,.NbPages=1};
#if defined(FLASH_DBANK_SUPPORT)
    if(FLASH_BANK_NB==2 && address-FLASH_BASE>=FLASH_BANK_SIZE) erase.Banks=FLASH_BANK_2;
#endif
    uint32_t failed_page=0; return HAL_FLASHEx_Erase(&erase,&failed_page)==HAL_OK;
}
static bool program(void *context,unsigned index,unsigned offset,const uint8_t data[8]) {
    (void)context; uint32_t address;
    if(!page_address(index,&address) || offset%8 || offset>OSG_JOURNAL_PAGE_SIZE-8) return false;
    uint64_t value; memcpy(&value,data,sizeof value);
    return HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,address+offset,value)==HAL_OK;
}
static const osg_journal_io_t io={.read=page,.erase=erase_page,.program=program};
bool board_registry_load(uint8_t *payload,size_t capacity,size_t *length,uint32_t *epoch) {
    bool loaded=osg_journal_load(&io,payload,capacity,length,epoch);
    if(!loaded && !osg_journal_erased(&io)) board_fault();
    return loaded;
}
bool board_registry_commit(const uint8_t *payload,size_t length,uint32_t epoch) {
    if(length>BOARD_REGISTRY_CAPACITY || !epoch || FLASH_SIZE!=128U*1024U || HAL_FLASH_Unlock()!=HAL_OK) return false;
    __HAL_FLASH_CLEAR_FLAG(FLASH_SR_CLEAR);
    bool ok=osg_journal_commit(&io,payload,length,epoch);
    (void)HAL_FLASH_Lock(); return ok;
}
