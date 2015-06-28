open(JOY,"/dev/joy0")||die;
while(1){
	sysread(JOY,$x,16);
	@j=unpack("iiii",$x);
	print "@j\n";sleep(1);
}
