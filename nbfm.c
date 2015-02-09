// verificare fattibilità di compensazione in temperatura
// capire bene deviazione in base a sampling che sembra 5@11kHz, 8@22, 10@44, 12@88         ossia 75@11, 150@88 in FM larga


// To compile:
// gcc -o3 -lm -std=c99 -o nbfm nbfm.c
// Derived from pifm, the great project by pihackfm
// Thanks to Mauro, IK1WVQ, for support


#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <math.h>
#include <fcntl.h>
#include <assert.h>
#include <malloc.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <unistd.h>

#define PAGE_SIZE (4*1024)
#define BLOCK_SIZE (4*1024)
#define PI2 6.2831853071

#define KNRM  "\x1B[0m"  //normal
//#define KRED  "\x1B[31m" //red
//#define KGRN  "\x1B[32m" //green
//#define KYEL  "\x1B[33m" //yellow
//#define KBLU  "\x1B[34m" //blue
//#define KMAG  "\x1B[35m" //magenta
#define KCYN  "\x1B[36m" //cyan
//#define KWHT  "\x1B[37m" //white
//#define RESET "\033[0m"  //reset
#define BOLDYELLOW "\033[1m\033[33m"
//#define ESC 27 //chr 27 escape
//#define BoldOn "%c[1m",27 //Bold activation
//#define BoldOf "%c[0m",27 //Bold deactivation

int  mem_fd;
int PreEmph;
float NominalFreq;
float CorrFreq;
float Deviation;
float Sampling;
char *gpio_mem, *gpio_map;
char *spi0_mem, *spi0_map;
float ToneFreq; //frequency in Hz of the tone for CTCSS or testing
float ToneLev;  //level of the tone. Example 0.001 for 1/1000 of the full scale peak2peak
float ToneSampling; // ToneFreq/samplerate ratio
char Power='7'; //Output power factor 0-7
				//Direct output from MY UNIT (at max level=7):
				//   1842 kHz >> +13.9dBm   160m
				//   3512 kHz >> +13.8dBm    80m
				//   7012 kHz >> +13.6dBm    40m
				//  10112 kHz >> +13.6dBm    30m
				//  14012 kHz >> +13.5dBm    20m
				//  18080 kHz >> +13.7dBm    17m
				//  21012 kHz >> +13.7dBm    15m
				//  24902 kHz >> +13.5dBm    12m
				//  28012 kHz >> +13.5dBm    10m
				
				//  On 10m (28012 kHz) the 8 levels are about:
				//  7 = +13.5dBm
				//  6 = +13.2dBm
				//  5 = +12.8dBm
				//  4 = +12.2dBm
				//  3 = +11.4dBm
				//  2 = +10.2dBm
				//  1 = + 8.0dBm
				//  0 = + 3.2dBm
				
float FreqDivider;
float FractFreq;
float DMAShift;
float DMAFreqCorr;
int IntFreqDivider;
int FractFreqDivider;
 

// I/O access
volatile unsigned *gpio;
volatile unsigned *allof7e;

// GPIO setup macros. Always use INP_GPIO(x) before using OUT_GPIO(x) or SET_GPIO_ALT(x,y)
#define INP_GPIO(g) *(gpio+((g)/10)) &= ~(7<<(((g)%10)*3))
#define OUT_GPIO(g) *(gpio+((g)/10)) |=  (1<<(((g)%10)*3))
#define SET_GPIO_ALT(g,a) *(gpio+(((g)/10))) |= (((a)<=3?(a)+4:(a)==4?3:2)<<(((g)%10)*3))

#define GPIO_SET *(gpio+7)  // sets   bits which are 1 ignores bits which are 0
#define GPIO_CLR *(gpio+10) // clears bits which are 1 ignores bits which are 0
#define GPIO_GET *(gpio+13)  // sets   bits which are 1 ignores bits which are 0

#define ACCESS(base) *(volatile int*)((int)allof7e+base-0x7e000000)
#define SETBIT(base, bit) ACCESS(base) |= 1<<bit
#define CLRBIT(base, bit) ACCESS(base) &= ~(1<<bit)

#define CM_GP0CTL (0x7e101070)
#define GPFSEL0 (0x7E200000)
#define CM_GP0DIV (0x7e101074)
#define PADS_GPIO_0_27  (0x7e10002c)
#define CLKBASE (0x7E101000)
#define DMABASE (0x7E007000)
#define PWMBASE  (0x7e20C000) /* PWM controller */



struct GPCTL {
    char SRC         : 4;
    char ENAB        : 1;
    char KILL        : 1;
    char             : 1;
    char BUSY        : 1;
    char FLIP        : 1;
    char MASH        : 2;
    unsigned int     : 13;
    char PASSWD      : 8;
};


void clearScreen() {
  const char* CLEAR_SCREE_ANSI = "\e[1;1H\e[2J";
  write(STDOUT_FILENO,CLEAR_SCREE_ANSI,12); }

void getRealMemPage(void** vAddr, void** pAddr) {
    void* a = valloc(4096);
    
    ((int*)a)[0] = 1;  // use page to force allocation.
    
    mlock(a, 4096);  // lock into ram.
    
    *vAddr = a;  // yay - we know the virtual address
    
    unsigned long long frameinfo;
    
    int fp = open("/proc/self/pagemap", 'r');
    lseek(fp, ((int)a)/4096*8, SEEK_SET);
    read(fp, &frameinfo, sizeof(frameinfo));
    
    *pAddr = (void*)((int)(frameinfo*4096));
}

void freeRealMemPage(void* vAddr) {
    
    munlock(vAddr, 4096);  // unlock ram.
    
    free(vAddr);
}

void setup_fm()
{

    /* open /dev/mem */
    if ((mem_fd = open("/dev/mem", O_RDWR|O_SYNC) ) < 0) {
        printf("can't open /dev/mem \n");
        exit (-1);
    }
    
    allof7e = (unsigned *)mmap(
                  NULL,
                  0x01000000,  //len
                  PROT_READ|PROT_WRITE,
                  MAP_SHARED,
                  mem_fd,
                  0x20000000  //base
              );

    if ((int)allof7e==-1) exit(-1);

    SETBIT(GPFSEL0 , 14);
    CLRBIT(GPFSEL0 , 13);
    CLRBIT(GPFSEL0 , 12);

 
    struct GPCTL setupword = {6/*SRC*/, 1, 0, 0, 0, 1,0x5a};

    ACCESS(CM_GP0CTL) = *((int*)&setupword);
	ACCESS(PADS_GPIO_0_27) = 0x5a000018 + (int)(Power-'0');  //set the output power
}


void modulate(int m)
{
    ACCESS(CM_GP0DIV) = (0x5a << 24) + 0x4d72 + m;
}

struct CB {
    volatile unsigned int TI;
    volatile unsigned int SOURCE_AD;
    volatile unsigned int DEST_AD;
    volatile unsigned int TXFR_LEN;
    volatile unsigned int STRIDE;
    volatile unsigned int NEXTCONBK;
    volatile unsigned int RES1;
    volatile unsigned int RES2;
    
};

struct DMAregs {
    volatile unsigned int CS;
    volatile unsigned int CONBLK_AD;
    volatile unsigned int TI;
    volatile unsigned int SOURCE_AD;
    volatile unsigned int DEST_AD;
    volatile unsigned int TXFR_LEN;
    volatile unsigned int STRIDE;
    volatile unsigned int NEXTCONBK;
    volatile unsigned int DEBUG;
};

struct PageInfo {
    void* p;  // physical address
    void* v;   // virtual address
};

struct PageInfo constPage;   
struct PageInfo instrPage;
struct PageInfo instrs[1024];


void playWav(char* filename, float samplerate)
{
    int fp= STDIN_FILENO;
	
    if(filename[0]!='-') fp = open(filename, 'r');
    //int sz = lseek(fp, 0L, SEEK_END);
    //lseek(fp, 0L, SEEK_SET);
    //short* data = (short*)malloc(sz);
    //read(fp, data, sz);
    
    int bufPtr=0;
    float datanew, dataold = 0;
    short data;
    float ToneData=0.0;
    float ToneLoop;
    float DataSample;
   
    int clocksPerSample = 22500.0/samplerate*1388.889;  // for timing (was 1400)
    float fmconstant  = samplerate * PreEmph / 1000000; // for pre-emphasys filter. 50us time constant WBFM, 400us NBFM

	//fprintf(stderr, "  FreqDivider=%.7f  FractFreq=%.4f\n",FreqDivider,FractFreq);
	//fprintf(stderr, "  DMAShift=%.7f  DMAFreqCorr=%.4f\n",DMAShift,DMAFreqCorr);
	//fprintf(stderr, "  IntFreqDivider=%i  FractFreqDivider=%i\n",IntFreqDivider,FractFreqDivider);

    for (int i=0; i<22; i++)
       read(fp, &data, 2);  // read past header
    
	ToneLoop=ToneSampling;
    while (read(fp, &data, 2)) {
        if (ToneLev>0) ToneData = ToneLev * sin (ToneLoop*PI2);
		DataSample= (1-ToneLev) * (float)(data)/32767;
        //fprintf(stderr,"Data=%i Loop=%.1f",ToneData,ToneLoop);
	//float fmconstant = samplerate * PreEmph / 1000000;  // moved outside this loop. For pre-emphisis filter.  50us time constant WBFM, 500us NBFM
        //int clocksPerSample = 22500.0/samplerate*1400.0;  // moved outside this while loop
        
        datanew = DataSample+ToneData;
        
        float sample = datanew + (dataold-datanew) / (1-fmconstant);  // fir of 1 + s tau
        float dval = (sample*Deviation) + DMAFreqCorr;  // actual transmitted sample.  15 is bandwidth (about 75 kHz), 1 for NBFM at 145MHz
        
        int intval = (int)(round(dval));  // integer component
        float frac = (dval - (float)intval)/2 + 0.5;
        unsigned int fracval = frac*clocksPerSample;
         
        bufPtr++;
        while( ACCESS(DMABASE + 0x04 /* CurBlock*/) ==  (int)(instrs[bufPtr].p)) usleep(1000);
        ((struct CB*)(instrs[bufPtr].v))->SOURCE_AD = (int)constPage.p + 2048 + intval*4 - 4 ;
        
        bufPtr++;
        while( ACCESS(DMABASE + 0x04 /* CurBlock*/) ==  (int)(instrs[bufPtr].p)) usleep(1000);
        ((struct CB*)(instrs[bufPtr].v))->TXFR_LEN = clocksPerSample-fracval;
        
        bufPtr++;
        while( ACCESS(DMABASE + 0x04 /* CurBlock*/) ==  (int)(instrs[bufPtr].p)) usleep(1000);
        ((struct CB*)(instrs[bufPtr].v))->SOURCE_AD = (int)constPage.p + 2048 + intval*4+4;
        
        bufPtr=(bufPtr+1) % (1024);
        while( ACCESS(DMABASE + 0x04 /* CurBlock*/) ==  (int)(instrs[bufPtr].p)) usleep(1000);
        ((struct CB*)(instrs[bufPtr].v))->TXFR_LEN = fracval;
        
        dataold = datanew;
		if (ToneLoop<1) {ToneLoop=ToneLoop+ToneSampling;} else {ToneLoop=ToneSampling;}
    }
    close(fp);
}

void unSetupDMA(){
    printf("exiting\n");
    struct DMAregs* DMA0 = (struct DMAregs*)&(ACCESS(DMABASE));
    DMA0->CS =1<<31;  // reset dma controller
    
}

void handSig() {
    exit(0);
}
void setupDMA( float centerFreq ){

  atexit(unSetupDMA);
  signal (SIGINT, handSig);
  signal (SIGTERM, handSig);
  signal (SIGHUP, handSig);
  signal (SIGQUIT, handSig);

   // allocate a few pages of ram
   getRealMemPage(&constPage.v, &constPage.p);
   
   int centerFreqDivider = (int)((500.0 / centerFreq) * (float)(1<<12) + 0.5);
   //fprintf(stderr,"\ncentreFreDiv=%i\n",centerFreqDivider); //for debug
   
   // make data page contents - it's essientially 1024 different commands for the
   // DMA controller to send to the clock module at the correct time.
   for (int i=0; i<1024; i++)
     ((int*)(constPage.v))[i] = (0x5a << 24) + centerFreqDivider - 512 + i;
   
   
   int instrCnt = 0;
   
   while (instrCnt<1024) {
     getRealMemPage(&instrPage.v, &instrPage.p);
     
     // make copy instructions
     struct CB* instr0= (struct CB*)instrPage.v;
     
     for (int i=0; i<4096/sizeof(struct CB); i++) {
       instrs[instrCnt].v = (void*)((int)instrPage.v + sizeof(struct CB)*i);
       instrs[instrCnt].p = (void*)((int)instrPage.p + sizeof(struct CB)*i);
       instr0->SOURCE_AD = (unsigned int)constPage.p+2048;
       instr0->DEST_AD = PWMBASE+0x18 /* FIF1 */;
       instr0->TXFR_LEN = 4;
       instr0->STRIDE = 0;
       //instr0->NEXTCONBK = (int)instrPage.p + sizeof(struct CB)*(i+1);
       instr0->TI = (1/* DREQ  */<<6) | (5 /* PWM */<<16) |  (1<<26/* no wide*/) ;
       instr0->RES1 = 0;
       instr0->RES2 = 0;
       
       if (i%2) {
         instr0->DEST_AD = CM_GP0DIV;
         instr0->STRIDE = 4;
         instr0->TI = (1<<26/* no wide*/) ;
       }
       
       if (instrCnt!=0) ((struct CB*)(instrs[instrCnt-1].v))->NEXTCONBK = (int)instrs[instrCnt].p;
       instr0++;
       instrCnt++;
     }
   }
   ((struct CB*)(instrs[1023].v))->NEXTCONBK = (int)instrs[0].p;
   
   // set up a clock for the PWM
   ACCESS(CLKBASE + 40*4 /*PWMCLK_CNTL*/) = 0x5A000026; //dec 1509949478
   usleep(1000);
   ACCESS(CLKBASE + 41*4 /*PWMCLK_DIV*/)  = 0x5A002800; //dec 1509959680
   ACCESS(CLKBASE + 40*4 /*PWMCLK_CNTL*/) = 0x5A000016; //dec 1509949462
   usleep(1000); 

   // set up pwm
   ACCESS(PWMBASE + 0x0 /* CTRL*/) = 0;
   usleep(1000);
   ACCESS(PWMBASE + 0x4 /* status*/) = -1;  // clear errors
   usleep(1000);
   ACCESS(PWMBASE + 0x0 /* CTRL*/) = -1; //(1<<13 /* Use fifo */) | (1<<10 /* repeat */) | (1<<9 /* serializer */) | (1<<8 /* enable ch */) ;
   usleep(1000);
   ACCESS(PWMBASE + 0x8 /* DMAC*/) = (1<<31 /* DMA enable */) | 0x0707;
   
   //activate dma
   struct DMAregs* DMA0 = (struct DMAregs*)&(ACCESS(DMABASE));
   DMA0->CS =1<<31;  // reset
   DMA0->CONBLK_AD=0; 
   DMA0->TI=0; 
   DMA0->CONBLK_AD = (unsigned int)(instrPage.p);
   DMA0->CS =(1<<0)|(255 <<16);  // enable bit = 0, clear end flag = 1, prio=19-16
}


void STOP_rf_output(int source)
{
/* open /dev/mem */
if ((mem_fd = open("/dev/mem", O_RDWR|O_SYNC) ) < 0)
	{
	printf("can't open /dev/mem \n");
	exit (-1);
	}
    
allof7e = (unsigned *)mmap( NULL, 0x01000000,  /*len */ PROT_READ|PROT_WRITE, MAP_SHARED, mem_fd, 0x20000000  /* base */ );

if ((int)allof7e==-1) exit(-1);

SETBIT(GPFSEL0 , 14);
CLRBIT(GPFSEL0 , 13);
CLRBIT(GPFSEL0 , 12);
 
struct GPCTL setupword = {source/*SRC*/, 0, 0, 0, 0, 1,0x5a};
ACCESS(CM_GP0CTL) = *((int*)&setupword);
}


void OffhandSig() {
     setupDMA(500);
     exit(0);
}


int main(int argc, char **argv)
{
    clearScreen();
	int clock_source = 6; //0=GND
	float ppM=0.0; //parts per Million frequency correction factor
	FILE *temperatureFile;
    double Temperature;
	//char TXLine[82];

	temperatureFile = fopen ("/sys/class/thermal/thermal_zone0/temp", "r");
	if (temperatureFile == NULL) Temperature=100.0;
	fscanf (temperatureFile, "%lf", &Temperature);
	fclose (temperatureFile);
	Temperature /= 1000;	
	//printf ("The temperature is %6.3f C.\n", Temperature); //for debug
	
	fprintf(stderr, "┌────────────────────────────────────────────────────────────────────────────┐\n");
    fprintf(stderr, "│" BOLDYELLOW "NBFM transmitter v20140807 by IK1PLD                                        " KNRM "│\n");
	fprintf(stderr, "├────────────────────────────────────────────────────────────────────────────┤\n");
	
	if (argc>1) {
      PreEmph=argc>6?atoi(argv[6]):400; //this is the Pre-Emphasys (default 400us);
      NominalFreq=argc>2?atof(argv[2]):29.6000; //this is still the uncorrected nominal frequency;
	  Sampling=argc>3?atof(argv[3]):11025; // this is the sample rate of the audio file
	  ppM=argc>4?atof(argv[4]):0.0;
      CorrFreq=NominalFreq+(NominalFreq*ppM/1000000); //now ppM correction applied
      Deviation=argc>5?atof(argv[5]):5; //this is the FM deviation (default 5kHz for ham NBFM)
	  Deviation=Deviation+(Deviation/1.6)*log(Sampling/11025.0); //this is the corrected deviation (depending on sample rate) 
      Deviation=(2368*Deviation)/(CorrFreq*CorrFreq); //2368 is the value corresponding to NBFM 5kHz deviation,
        // 16kHz channel BW;  (Deviation*log(Sampling/11025.0)/1.6) parameter related to sample rate
	  ToneLev=argc>7?atof(argv[7]):0.000;//this is the level for the audio tone to be added to the modulation
	  ToneFreq=argc>8?atof(argv[8]):110.9;//this is the frequency for the audio tone to be added to the modulation
	  Power=argc>9?*argv[9]:'7';//this is the output power
	  if((Power<'0') || (Power>'7')) Power='7';
	  
	  setup_fm();
	  
	     FreqDivider = 500.0 / CorrFreq; //frequency divisor, decimal
         IntFreqDivider = (int)FreqDivider; // integer part of the frequency divisor
         FractFreqDivider =(int)(((FreqDivider - IntFreqDivider) * 4096.0) + 0.5); //fractional part of the frequency divisor
         FractFreq = 500.0 / (IntFreqDivider + (FractFreqDivider / 4096.0)); //fractional frequency without DMA shifting
		 DMAShift = 1000.0 * (FractFreq - CorrFreq); // DMA frequency shift
         DMAFreqCorr = (2300.0 * DMAShift) / (CorrFreq * CorrFreq); //DMA Frequency shift factor
		 
		 //int centerFreqDivider = (int)((500.0 / CorrFreq) * (float)(1<<12) + 0.5);//for debug
		 //fprintf(stderr,"\ncenterFreDivMAIN=%i\n",centerFreqDivider);//for debug
		 //fprintf(stderr,"\ncenterFrequencyDivFrequency=%8.4f\n",4096.0*500.0/centerFreqDivider);//for debug

		 setupDMA(CorrFreq);

		 //strcpy(greeting, "Hello World");
		 fprintf(stderr, "│Transmitting %s in NBFM on %.4f MHz  with %.1f ppM correction\n",argv[1], NominalFreq ,ppM);
	     //fprintf(stderr, "\nCalcolo stringa = %i\n\n", (int)20-(FreqCorr>=1?strlen(argv[1]):0)-(int)(log10(FreqCorr))-(ppM>0?(int)(log10(ppM)):0));
	     //for (int i=0; i<20-strlen(argv[1])-(int)(log10(FreqCorr))-(int)(log10(ppM)); i++) fprintf(stderr, " ");
      fprintf(stderr, "│Fractional divider frequency=%8.4f MHz      DMA Shift=%7.4f MHz        │\n", FractFreq,DMAShift/1000.0);
      fprintf(stderr, "│Deviation factor=%10.1f                    Power factor=%c               │\n",Deviation,Power);
	  if (ToneLev>0) fprintf(stderr, "│Adding Tone frequency %5.1f Hz,  level %5.3f                                │\n",ToneFreq, ToneLev);
	  fprintf(stderr, "└────────────────────────────────────────────────────────────────────────────┘\n");

	  ToneSampling=1.004*ToneFreq/Sampling;//1.004 factor for subtones frequency centering with 1388.889c/s
	  //fprintf(stderr, "ToneSampling %.4f\n",ToneSampling);
        signal (SIGINT, OffhandSig);
        signal (SIGTERM, OffhandSig);
        signal (SIGHUP, OffhandSig);
        signal (SIGQUIT, OffhandSig);
      playWav(argv[1], Sampling);
      //setupDMA(500);
	  STOP_rf_output(clock_source);
	  //fflush(stdout);	
	  // fprintf(stderr, "\n");
    } else {
      fprintf(stderr, "│Usage:                                                                      │\n"); 
	  fprintf(stderr, "│" KCYN "%s afile.wav [freq [sampling [ppM [dev [p-emph [ToneL [ToneF] [Power]   " KNRM "│\n",argv[0]); //0 for program name
	  fprintf(stderr, "│                                                                            │\n");
	  fprintf(stderr, "│Where:                                                                      │\n");
	  fprintf(stderr, "│" KCYN "afile" KNRM " is audio 16 bit Mono.  Set wavfile to '-' to use stdin.               │\n"); //1
	  fprintf(stderr, "│" KCYN "freq" KNRM " is carrier frequency in Mhz (default 29.6)                             │\n"); //2
	  fprintf(stderr, "│" KCYN "sampling" KNRM " is sample rate of wav file in Hz (default 11025)                   │\n"); //3
	  fprintf(stderr, "│" KCYN "ppM" KNRM " is the frequency correction in parts per Million (default 0)            │\n"); //4
	  fprintf(stderr, "│" KCYN "Deviation" KNRM " in kHz to set FM index (default 5)                                │\n"); //5
	  fprintf(stderr, "│" KCYN "pre-emphasys" KNRM " in us (default 400)                                            │\n"); //6
	  fprintf(stderr, "│" KCYN "ToneL" KNRM " is the tone level (default 0.0)                                       │\n"); //7
	  fprintf(stderr, "│" KCYN "ToneF" KNRM " is the tone frequency in Hz for CTCSS or tests (default: 110.9)       │\n"); //8
	  fprintf(stderr, "│" KCYN "Power" KNRM " is the output power factor, between 0 and 7 (default: 7)              │\n"); //9
	  fprintf(stderr, "│                                                                            │\n");
	  fprintf(stderr, "│Set deviation to 0 to transmit silence (or for frequency calibration)       │\n");
	  fprintf(stderr, "└────────────────────────────────────────────────────────────────────────────┘\n");
           }
	   
    return 0;

} // main
