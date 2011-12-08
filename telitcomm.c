//******************************************************************************
//   Telit Communications Manager for Project Bacon
//
//   Description: Initializes Telit GE865-QUAD and posts datapacket to server
//   USCI_A0 RX interrupt triggers fills circular buffer
//   ACLK = BRCLK = LFXT1 = 32768, MCLK = SMCLK = DCO~1048k
//   //* An external watch crystal is required on XIN XOUT for ACLK *//
//
//            |     P4.7/UCA0RXD|------------>
//            |                 | 4800 - 8N1
//            |     P4.6/UCA0TXD|<------------
//
//   R. von Kurnatowski III
//   TX/RX Labs
//   November 2011
//   Built with CCE Version: 4.0.1
//******************************************************************************
#include  "msp430xG46x.h"
#include "telitcomm.h"
#include <stdlib.h>
/*  Global Variables  */
unsigned char resp[40];
volatile unsigned char ucRXBuffer[32];
volatile unsigned char ucWriteIndex = 0;
volatile unsigned char ucReadIndex = 0;
volatile unsigned char timedout;
gprs_state_t gprs_state = STATE_UNINIT;

static unsigned char sgactt1[] = "#";
static unsigned char sgactt2[] = "\n0";
static char *atstr[]           = {"AT\r",0};
static telitcmd at             = {atstr,&ackOK};
static char *atechostr[]       = {"ATE\r",0};
static telitcmd atecho         = {atechostr, &ackOK};
static char *atvstr[]          = {"ATV0\r",0};
static telitcmd atv            = {atvstr, &ackOK};
static char *atqstr[]          = {"ATQ0\r",0};
static telitcmd atq            = {atqstr, &ackOK};
static char *atflowctlstr[]    = {"AT&K=0\r",0};
static telitcmd atflowctl      = {atflowctlstr, &ackOK};
static char *atsetusidstr[]    = {"AT#USERID=\"WAP@CINGULARGPRS.COM\"\r",0};
static telitcmd atsetusid      = {atsetusidstr, &ackOK};
static char *atsetpassstr[]    = {"AT#PASSW=\"CINGULAR1\"\r",0};
static telitcmd atsetpass      = {atsetpassstr, &ackOK};
static char *atconnectstr[]    = {"AT#SGACT=1,1\r",0};
static telitcmd atconnect      = {atconnectstr, &ackSGACT};
static char *atopenscktstr[]   = {"AT#SD=1,0,3000,\"","50.17.231.113","\",255\r",0};
static telitcmd atopensckt     = {atopenscktstr, &ackSD};
static char *atdisconnectstr[] = {"AT#SGACT=1,0\r",0};
static telitcmd atdisconnect   = {atdisconnectstr, &ackOK};
static char *atcgdcontstr[]    = {"AT+CGDCONT=1,\"IP\",\"WAP.CINGULAR\"\r", 0};
static telitcmd atcgdcont      = {atcgdcontstr, &ackOK};
static char *atclosestr[]      = {"AT#SH=1\r",0};
static telitcmd atclose        = {atclosestr, &ackOK};

#pragma vector=USCIAB0RX_VECTOR
__interrupt void USCIA0RX_ISR (void)
{
	ucRXBuffer[ucWriteIndex++] = UCA0RXBUF; // store received byte and
	ucWriteIndex &= 0x1f; // reset index

}

unsigned char isRXBufferEmpty (void) {
	return (ucWriteIndex == ucReadIndex);
}

unsigned char getRXBufferChar (void) {
	unsigned char Byte;
	if (ucWriteIndex != ucReadIndex) { // char still available
		Byte = ucRXBuffer[ucReadIndex++]; // get byte from buffer
		ucReadIndex &= 0x1f; // adjust index
		return (Byte);
	} else return (0); // if there is no new char
}

void clearRXBuffer() {
	IE2 &= ~UCA0RXIE;                          // Disable USCI_A0 RX interrupt
	ucReadIndex=0;
	ucWriteIndex = 0;
	IE2 |= UCA0RXIE;                          // Enable USCI_A0 RX interrupt
}

void do_init() {
	P4SEL |= 0x0C0;
	// P4.7,6 = USCI_A0 RXD/TXD
	UCA0CTL1 |= UCSWRST;
	UCA0CTL1 |= UCSSEL_1;                     // CLK = ACLK
	UCA0BR0 = 0x06;                           // 32k/9600 - 3.41
	UCA0BR1 = 0x00;                           //
	UCA0MCTL = UCBRS2 +UCBRS1+ UCBRS0;                          // Modulation
	UCA0CTL1 &= ~UCSWRST;                     // **Initialize USCI state machine**
}

unsigned int do_open() {
	return 0;
}

void do_uninit() {
	UCA0CTL1 |= UCSWRST;
}
unsigned int do_close() {
	return 0;
}

unsigned int gprsSend(unsigned char *data, unsigned int len) {
	switch (gprs_state) {
		case STATE_UNINIT:
			do_init();
		case STATE_INIT:
			if(do_open()) return 1;
		}
		return sendData(data, len);
}

unsigned int gprsClose() {

	switch (gprs_state) {
		case STATE_OPEN:
		case STATE_ERROR:
			do_close();
		case STATE_INIT:
			do_uninit();
		case STATE_UNINIT:
			return 0;
	}
}

unsigned int ackOK(void) {
	getResponse();
	return  memcmp(resp, "0", 2);
}
unsigned int ackSGACT(void) {
	getResponse();

	if(memcmp(resp, sgactt1, 1)) {
		__no_operation();
		return 1;
	}
	getResponse();
	if(memcmp(resp, sgactt2, 2)) {
		__no_operation();
		return 1;
	}
	__no_operation();
	return 0;
}

unsigned int ackSend(void) {
	unsigned int isbody = 1;
	while(isbody) {
		getResponse();
		isbody = !(memcmp(resp, "0", 2));
	}
	return 0;
}
unsigned int ackSD(void) {
	getResponse();
	return memcmp(resp, "1", 2);
}

unsigned int sendData(unsigned char *data, unsigned int len) {
	unsigned int pos = 0;
	while(pos < len) {
		while(!(IFG2&UCA0TXIFG));
					UCA0TXBUF = *(data+pos);
					pos++;
	}
	return 0;
}

unsigned int sendCmd(telitcmd tcmd) {

	unsigned int ret;
	unsigned int pos=0;
	IE2 |= UCA0RXIE;                          // Enable USCI_A0 RX interrupt
	clearRXBuffer();
	while(*(tcmd.cmd+pos) != NULL) {
		char *c = *(tcmd.cmd+pos);
		pos++;
		while(*c!='\0') {
			while(!(IFG2&UCA0TXIFG));
			UCA0TXBUF = *c;
			c++;
		}
	}
	ret = tcmd.callback();
	IE2 &= ~UCA0RXIE;                          // Enable USCI_A0 RX interrupt
	return ret;

}
unsigned char getResponse() {
	unsigned char ptr = 0;
	timedout = 0;
	unsigned char c;
	while(!timedout) {
		if (!isRXBufferEmpty()) {
			c = getRXBufferChar();
			if(c=='\r' || ptr>38) {
				*(resp+ptr) = '\0';
				return 0;
			} else *(resp+(ptr++)) = c;
		}
	}
	return 1;
}
