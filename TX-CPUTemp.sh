#!/bin/bash

#creates a 10MB RamDisk
if [ ! -e /mnt/ramdisk ]; then
     mkdir -p /mnt/ramdisk
fi
mount -t ramfs -o size=10m ramfs /mnt/ramdisk

cp nbfm /mnt/ramdisk/nbfm
cp TX-CPUTemp.sh /mnt/ramdisk/TX-CPUTemp.sh
cp pkt2wave /mnt/ramdisk/pkt2wave

cd /mnt/ramdisk

rm -f CPUTemp.txt
rm -f CPUTemp_pkt.txt

#activates GPIO22 port (pin15) as pull-upped Input
#you can choose any available ports you wish and use them as signal inputs or alarms
gpio -g mode 22 up

#activates GPIO17 port (pin11) as output to light on a led indicating RF output or to switch amplifiers, filters etc.
gpio -g mode 17 out
gpio -g write 17 0

clear

inp1=$1
inp2=$2

if [ -z $1 ]; then
	echo ""
	echo "NBFM Transmission of the CPU temp, load and time, v20140901 by IK1PLD"
	echo ""
	echo "Use: ./TX-CPUTemp.sh Freq [Interval [ppM [dev [p-emph [ToneL [ToneF [Pow]"
	echo "Freq: carrier frequency in MHz (example 145.3125)"
    echo "Interval: time interval, in sec, between transmissions (default 60)"
	echo "run ./nbfm for further information"	
	exit
fi

if [ -z $1 ]; then
    #default not used. It is mandatory to input a frequency in the command line.
	inp1=145.3125
fi

if [ -z $2 ]; then
    #set here the default interval in seconds
    inp2=60
fi

if [ -z $3 ]; then
    #set here the ppM correction for your own device
    inp3=-21
fi

if [ -z $4 ]; then
    #set here the default deviation
    inp4=5
fi

if [ -z $5 ]; then
    #set here the default pre-emphasys in microseconds
    inp5=400
fi

if [ -z $6 ]; then
    #set here the default Tone Level in absolute (between 0 and 1)
    inp6=0
fi

if [ -z $7 ]; then
    #set here the default Tone Frequency in Hz
    inp7=110.9
fi

if [ -z $8 ]; then
    #set here the default output Power (0-7)
    inp8=7
fi

echo ""
echo "CPU temperature and load is transmitted on $inp1 MHz every $inp2 seconds"
echo "A logger file named /mnt/ramdisk/Logger.txt is also temporary saved onto the ramdisk"
echo ""
startN=1

while [ $startN -le 1000 ];
do
        temp_gpu=$(/opt/vc/bin/vcgencmd measure_temp | tr -d [a-z,A-Z,"=","'","."][])
		cpuUsageM=$(top -bn 1 | awk '{print $9}' | tail -n +8 | awk '{s+=$1} END {print s}')
			cpuTempString=$(cat /sys/class/thermal/thermal_zone0/temp)
			cpuTempInt=$(($cpuTempString/1000))
			cpuTempDec=$(($cpuTempString/100))
			cpuTempDec=$(($cpuTempDec % $cpuTempInt))
			cpuTemp=$cpuTempInt$cpuTempDec
		let cpuTemp=(${cpuTemp}+${temp_gpu})/2
		TempCoreInt=${cpuTemp:0:2}
        TempCoreDec=${cpuTemp:2}
	Time=$(date +"%R")
	Seconds=$(date +"%S")
	Day=$(date +"%F")
    GPIO22=$(gpio -g read 22)	
	echo "Core Temperature is ${TempCoreInt}.${TempCoreDec} degrees, load is ${cpuUsageM} %, . at ${Time}" >> CPUTemp.txt
	echo "IK1PLD>BEACON:Temperature=${TempCoreInt}.${TempCoreDec}°C, load is ${cpuUsageM}%, at ${Time}" >> CPUTemp_pkt.txt

    if [ $GPIO22 == 0 ]; then 
        #chech the GPIO22 as input for signals or alarms, and modify the following text as you wish
        echo " . Input 22 active!!!" >> CPUTemp.txt
		echo ". Input 22 active!!!" >> CPUTemp_pkt.txt
    fi	
	
	echo "${Day};${Time}:${Seconds};${TempCoreInt}.${TempCoreDec};${cpuUsageM};${GPIO22};" >> Logger.txt
    cat CPUTemp.txt

	#the following line is to generate the voice audio file
		text2wave CPUTemp.txt -F 22500 -o CPUTemp.wav

	#the following line is to generate the packet radio audio file
		./pkt2wave CPUTemp_pkt.txt -n 1 -r 22500 -o CPUTemp_pkt.wav

	gpio -g write 17 1

	#the following two lines are for testing. Please comment them if you want to use ./nbfm FM transmitter
		aplay CPUTemp.wav
		aplay CPUTemp_pkt.wav

	#the following line is to transmit voice messages. Digit ./nbfm for help
		#./nbfm CPUTemp.wav $inp1 22500 $inp3 $inp4 $inp5 $inp6 $inp7 $inp8
		
	#the following line is to transmit packet radio.
		#./nbfm CPUTemp-pkt.wav $inp1 44100 $inp3 $inp4 $inp5 $inp6 $inp7 $inp8

	gpio -g write 17 0
    rm -f CPUTemp.txt
	rm -f CPUTemp_pkt.txt
	let startN=$startN+1
	sleep $inp2
done

echo ""
echo "End of transmissions."
exit
