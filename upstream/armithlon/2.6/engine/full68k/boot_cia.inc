/* Compatibility for the stock Amiga ROM until a native timer.device is installed.
 * No guest task or TimerBase memory is modified. Display remains RTG-only. */
static void cia_irq(QRXFullCPU *c,unsigned chip) {
    QRXBootCIA *a=&c->boot_cia[chip];
    if(a->pending&a->mask)c->boot_intreq|=chip?0x2000:0x0008;
    paula_recompute(c);
}
static int cia_address(uint32_t addr,unsigned *reg) {
    if(addr>=0xbfd000 && addr<=0xbfdfff && !(addr&1)) {*reg=(addr>>8)&15;return 1;}
    if(addr>=0xbfe000 && addr<=0xbfefff && (addr&1)) {*reg=(addr>>8)&15;return 0;}
    return -1;
}
static int disk_unit(QRXFullCPU *c) {
    for(int i=0;i<4;i++)if(!(c->ciab_prb&(8u<<i)))return i;
    return -1;
}
static bool amiga_cia_read(QRXFullCPU *c,uint32_t addr,unsigned width,uint32_t *out) {
    unsigned r;int chip=cia_address(addr,&r);
    if(chip<0 || width!=1)return false;
    QRXBootCIA *a=&c->boot_cia[chip];uint8_t v=a->reg[r];
    if(chip==0 && r==0) {
        v=(c->ciaa_pra&3)|0xfc; /* input pins stay pulled high, regardless of PRA output latch */
        int u=disk_unit(c);
        if(u==0 && c->adf_data) {
            v&=~0x08u; /* read-only inserted disk */
            if(!(c->ciab_prb&0x80))v&=~0x20u;
            else if(c->adf_size==1802240 && (c->boot_drive_id_bit&1))v&=~0x20u;
            if(c->fl_cyl[u]==0)v&=~0x10u;
        }
    } else if(chip==1 && r==1)v=c->ciab_prb;
    else if(r>=4 && r<=7){unsigned t=(r-4)/2;uint16_t n=(uint16_t)a->remaining[t];v=(r&1)?n>>8:n;}
    else if(r>=8 && r<=10) {
        uint32_t tod=chip?(uint32_t)(c->cycles/454):(uint32_t)(c->cycles/(454*313));
        if(r==10){a->tod_latched=tod;a->tod_hold=true;}
        v=(uint8_t)((a->tod_hold?a->tod_latched:tod)>>(8*(r-8)));
        if(r==8)a->tod_hold=false;
    } else if(r==13){v=a->pending|((a->pending&a->mask)?0x80:0);a->pending=0;}
    *out=v;return true;
}
static bool amiga_cia_write(QRXFullCPU *c,uint32_t addr,unsigned width,uint32_t val) {
    unsigned r;int chip=cia_address(addr,&r);
    if(chip<0 || width!=1)return false;
    QRXBootCIA *a=&c->boot_cia[chip];uint8_t v=(uint8_t)val;
    if(env_QRX_FULL_HW_TRACE){static unsigned n;if(n++<400)fprintf(stderr,"[CIA] %d reg=%x val=%02x cyc=%llu\n",chip,r,v,(unsigned long long)c->cycles);}
    if(chip==0 && r==0){c->ciaa_pra=v;c->rom_overlay=(v&1)!=0;}
    if(chip==1 && r==1){
        if(!(v&0x80))c->boot_drive_id_bit=0;
        else if((c->ciab_prb&8) && !(v&8))c->boot_drive_id_bit++;
        if(!(c->ciab_prb&1) && (v&1))for(int u=0;u<4;u++)if(!(v&(8u<<u))) {
            if(v&2){if(c->fl_cyl[u]>0)c->fl_cyl[u]--;}
            else if(c->fl_cyl[u]<85)c->fl_cyl[u]++;
        }
        c->ciab_prb=v;
    }
    if(r>=4 && r<=7){
        unsigned t=(r-4)/2;
        if(r&1)a->latch[t]=(a->latch[t]&255)|((uint16_t)v<<8);
        else a->latch[t]=(a->latch[t]&0xff00)|v;
        if(r&1){
            if(!(a->reg[14+t]&1))a->remaining[t]=a->latch[t];
            if(a->reg[14+t]&8){a->remaining[t]=a->latch[t];a->reg[14+t]|=1;}
        }
    } else if(r==13){
        if(v&0x80)a->mask|=v&31;else a->mask&=~(v&31);
        cia_irq(c,chip);return true;
    } else if(r==14 || r==15){
        if(v&16)a->remaining[r-14]=a->latch[r-14];
        v&=~16u;
    }
    a->reg[r]=v;return true;
}
static void boot_cia_tick(QRXFullCPU *c) {
    uint64_t now=c->cycles/10, ticks=now-c->boot_cia_clock;c->boot_cia_clock=now;
    for(unsigned chip=0;chip<2;chip++){
        QRXBootCIA *a=&c->boot_cia[chip];
        for(unsigned t=0;t<2;t++){
            if(!(a->reg[14+t]&1))continue;
            /* E-clock input only; chained/CNT timer modes remain unsupported. */
            if(a->reg[14+t]&(t?0x60:0x20))continue;
            uint64_t n=ticks;
            while(n>a->remaining[t]){
                n-=a->remaining[t]+1u;a->remaining[t]=a->latch[t];a->pending|=1u<<t;
                if(a->reg[14+t]&8){a->reg[14+t]&=~1u;n=0;break;}
            }
            a->remaining[t]-=(uint32_t)n;
        }
        cia_irq(c,chip);
    }

}
