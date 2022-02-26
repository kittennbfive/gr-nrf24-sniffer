#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <err.h>
#include <sys/time.h>
#include <getopt.h>
#include <ctype.h>
#include <signal.h>

/*
nrf-decoder version 1 (c) 2022 by kittennbfive

https://github.com/kittennbfive/

see README.md

AGPLv3+ and NO WARRANTY!
*/

//PCF (packet control field) is used by default but can be disabled for compatibility with older transceivers by setting EN_AA=0x00 (auto-ack disabled) and ARC=0 (auto-retransmit disabled). See datasheet of nRF24L01+ section 7.10.
typedef enum
{
	MODE_NORMAL, //default
	MODE_COMPATIBILITY //no PCF (packet control field), no auto-ack, no auto-retransmit //--mode-compatibility
} nrfmode_t;

typedef enum
{
	PAYLOAD_FIXED_LENGTH, //default
	PAYLOAD_DYNAMIC_LENGTH //--dyn-lengths
} payloadlengthmode_t;

typedef enum
{
	CRC_ONE_BYTE, //default
	CRC_TWO_BYTES //--crc16
} crcmode_t;

typedef enum //--disp [verbose|retransmits|none]
{
	DISP_VERBOSE,
	DISP_RETRANSMITS_ONLY,
	DISP_SUMMARY, //default
	DISP_NONE
} dispmode_t;

typedef enum //--dump-payload [data|ack|all]
{
	DUMP_OFF, //default
	DUMP_PACKET_PAYLOAD,
	DUMP_ACK_PAYLOAD,
	DUMP_PACKET_AND_ACK_PAYLOAD
} dumpmode_t;

typedef enum
{
	FILTER_PROMISCUOUS_MODE, //default
	FILTER_BY_ADDRESS //--filter-addr $addr_in_hex
} filtermode_t;

static nrfmode_t nrfmode=MODE_NORMAL;
static payloadlengthmode_t payloadlengthmode=PAYLOAD_FIXED_LENGTH;
static crcmode_t crcmode=CRC_ONE_BYTE;

static dispmode_t dispmode=DISP_SUMMARY;
static dumpmode_t dumpmode=DUMP_OFF;
static filtermode_t filtermode=FILTER_PROMISCUOUS_MODE;

static uint8_t samples_per_bit=0; //--spb $samples_per_bit MANDATORY

static uint8_t sz_addr_bytes=0; //--sz-addr $sz MANDATORY
static uint8_t filter_by_address[5]; //max 5 bytes by specification, usage see filtermode_t

static uint8_t sz_payload_bytes=0; //--sz-payload $sz, must be >=1
static uint8_t sz_ack_payload_bytes=0; //--sz-ack-payload $sz, can be 0!
static bool sz_ack_payload_bytes_specified=false;

//do not change - hardcoded by specification
#define SZ_ADDR_BYTES_MAX 5
#define NB_DATA_BYTES_MAX 32
#define BITS_PREAMBLE 8
#define BITS_PCF 9
#define CRC8_POLY 0x07
#define CRC16_POLY 0x1021
#define BUF_CRC_MAX (SZ_ADDR_BYTES_MAX+NB_DATA_BYTES_MAX+2) //2 bytes for PCF
#define MAX_PACKET_LENGTH_SAMPLES (8*(1+SZ_ADDR_BYTES_MAX+2+NB_DATA_BYTES_MAX+2)*samples_per_bit) //1 for preamble, 2 for PCF, 2 for CRC

#define SZ_BUFFER_SAMPLES (4*MAX_PACKET_LENGTH_SAMPLES) //4 randomly choosen, seems to work fine

//internal stuff
typedef enum
{
	PACKET_INVALID,
	PACKET_DATA_PACKET,
	PACKET_ACK_PACKET,
	PACKET_UNDISTINGUISHABLE
} packettype_t;

typedef enum
{
	PACK_DATA_FIXED_LENGTH,
	PACK_DATA_VAR_LENGTH,
	PACK_DATA_DONT_PACK
} packmode_data_t;

typedef struct
{
	uint8_t addr[SZ_ADDR_BYTES_MAX];
	struct
	{
		uint8_t payload_length; //this is only valid if dynamic payload length is enabled!
		uint8_t pid;
		bool no_ack;
	} pcf;
	uint8_t sz_payload_bytes;
	uint8_t payload[NB_DATA_BYTES_MAX];
	union
	{
		uint8_t crc8;
		uint16_t crc16;
	} crc;
} nRF24_packet_t;

#define BITS_TO_SAMPLES(nb) (nb*samples_per_bit)
#define BYTES_TO_SAMPLES(nb) (8*BITS_TO_SAMPLES(nb))

static uint8_t * ringbuffer;
static uint32_t nb_samples=0;
static uint32_t write_index=0;
static uint32_t read_index=0;

void ringbuffer_put_sample(const uint8_t byte)
{
	if(nb_samples++==SZ_BUFFER_SAMPLES)
		errx(1, "ring buffer overflow");
	ringbuffer[write_index++]=byte;
	write_index%=SZ_BUFFER_SAMPLES;	
}

uint8_t ringbuffer_get_sample_at_pos(const uint32_t pos)
{
	if(pos>nb_samples)
		errx(1, "ring buffer out of range (requested position %u but only %u samples in buffer)", pos, nb_samples);
	
	uint8_t byte=ringbuffer[(read_index+pos)%SZ_BUFFER_SAMPLES];
	return byte;
}

void ringbuffer_remove_samples(const uint32_t nb)
{
	if(nb>nb_samples)
		errx(1, "ring buffer underflow (requested removal of %u samples but only %u in buffer)", nb, nb_samples);

	read_index=(read_index+nb)%SZ_BUFFER_SAMPLES;
	nb_samples-=nb;
}

uint8_t get_bits(const uint32_t startpos_samples, const uint8_t nb_bits)
{
	if(nb_bits>8)
		errx(1, "get_bits: nb_bits must be <=8");
	
	uint8_t byte=0;
	uint8_t bitnr;
	
	for(bitnr=0; bitnr<nb_bits; bitnr++)
	{
		byte<<=1;
		if(ringbuffer_get_sample_at_pos(startpos_samples+BITS_TO_SAMPLES(bitnr)+samples_per_bit/2)) //reading at middle of bit
			byte|=1;
	}
	
	return byte;
}

uint8_t get_byte(const uint32_t startpos_samples)
{
	return get_bits(startpos_samples, 8);
}

bool check_for_preamble(void) //preamble can be 0x55 or 0xAA depending on address
{
	uint8_t i;
	bool bit;
	
	if(ringbuffer_get_sample_at_pos(0)==0)
	{
		for(i=0, bit=0; i<8; i++, bit=!bit)
		{
			if(ringbuffer_get_sample_at_pos(samples_per_bit/2+i*samples_per_bit)!=bit)
				return false;
		}
		return true;
	}
	else 
	{
		for(i=0, bit=1; i<8; i++, bit=!bit)
		{
			if(ringbuffer_get_sample_at_pos(samples_per_bit/2+i*samples_per_bit)!=bit)
				return false;
		}
		return true;
	}
}

void read_bytes(const uint32_t startpos_samples, const uint8_t nb, uint8_t * const dst)
{
	uint8_t i;
	for(i=0; i<nb; i++)
		dst[i]=get_byte(startpos_samples+BYTES_TO_SAMPLES(i));
}

uint8_t calc_crc8(uint8_t const * const data, const uint16_t sz_bits)
{
	uint8_t crc=0xff;
	uint8_t i;
	int8_t j;
	uint8_t k;
	
	uint16_t sz=sz_bits/8;
	uint8_t remainder=sz_bits-8*sz;
	
	for(i=0; i<sz; i++)
	{
		for(j=7; j>=0; j--)
		{
			if(((crc>>7)&1)!=((data[i]>>j)&1))
				crc=(crc<<1)^CRC8_POLY;
			else
				crc=crc<<1;		
		}
	}

	for(k=0,j=7; k<remainder; k++,j--)
	{
		if(((crc>>7)&1)!=((data[i]>>j)&1))
			crc=(crc<<1)^CRC8_POLY;
		else
			crc=crc<<1;
	
	}
	
	return crc;
}

uint16_t calc_crc16(uint8_t const * const data, const uint16_t sz_bits)
{
	uint16_t crc=0xffff;
	uint8_t i;
	int8_t j;
	uint8_t k;
	
	uint16_t sz=sz_bits/8;
	uint8_t remainder=sz_bits-8*sz;
	
	for(i=0; i<sz; i++)
	{
		for(j=7; j>=0; j--)
		{
			if(((crc>>15)&1)!=((data[i]>>j)&1))
				crc=(crc<<1)^CRC16_POLY;
			else
				crc=crc<<1;		
		}
	}

	for(k=0,j=7; k<remainder; k++,j--)
	{
		if(((crc>>15)&1)!=((data[i]>>j)&1))
			crc=(crc<<1)^CRC16_POLY;
		else
			crc=crc<<1;
	}
	
	return crc;
}

void read_pcf(uint32_t startpos_samples, nRF24_packet_t * const packet) //packet control field
{
	packet->pcf.payload_length=get_bits(startpos_samples, 6);
	startpos_samples+=BITS_TO_SAMPLES(6);
	packet->pcf.pid=get_bits(startpos_samples, 2);
	startpos_samples+=BITS_TO_SAMPLES(2);
	packet->pcf.no_ack=!!get_bits(startpos_samples, 1);
	startpos_samples+=BITS_TO_SAMPLES(1);
}

uint16_t pack_for_crc(uint8_t * const buf, nRF24_packet_t const * const packet, const uint8_t length_payload)
{
	uint8_t i,j;
	
	uint16_t bits_total=0;
	
	for(i=0,j=0; i<sz_addr_bytes; i++,j++)
	{
		buf[j]=packet->addr[i];
		bits_total+=8;
	}

	if(nrfmode==MODE_NORMAL) //has PCF
	{
		buf[j++]=(packet->pcf.payload_length<<2)|packet->pcf.pid;
	
		uint8_t remaining_bit=packet->pcf.no_ack;
		
		bits_total+=BITS_PCF;

		for(i=0; i<length_payload; i++,j++)
		{
			buf[j]=(remaining_bit<<7)|(packet->payload[i]>>1);
			remaining_bit=packet->payload[i]&1;
			bits_total+=8;
		}

		buf[j]=(remaining_bit<<7);
		
	}
	else
	{
		for(i=0; i<length_payload; i++,j++)
		{
			buf[j]=packet->payload[i];
			bits_total+=8;
		}
	}
	
	return bits_total;
}

void disp_packet_verbose(nRF24_packet_t const * const packet, struct timeval const * const timestamp, const packettype_t packettype, const bool is_retransmit)
{
	uint8_t i;
	
	fprintf(stderr, "[%10lu.%06lu] ", timestamp->tv_sec, timestamp->tv_usec);
	
	if(is_retransmit)
		fprintf(stderr, "[RETRANSMIT] ");
	
	if(packettype==PACKET_DATA_PACKET)
		fprintf(stderr, "data-packet addr=");
	else if(packettype==PACKET_ACK_PACKET)
		fprintf(stderr, "ACK-packet addr="); 
	else
		fprintf(stderr, "packet addr=");
	
	for(i=0; i<sz_addr_bytes; i++)
		fprintf(stderr, "%02x ", packet->addr[i]);
	
	fprintf(stderr, "PID=%u ", packet->pcf.pid);
	
	if(packettype==PACKET_DATA_PACKET && nrfmode==MODE_NORMAL)
		fprintf(stderr, "NO_ACK=%u ", packet->pcf.no_ack);
	
	if(packet->sz_payload_bytes!=0)
	{
		fprintf(stderr, "data[%u]=", packet->sz_payload_bytes);
		for(i=0; i<packet->sz_payload_bytes; i++)
			fprintf(stderr, "%02x ", packet->payload[i]);
	}
	
	if(crcmode==CRC_ONE_BYTE)
		fprintf(stderr, "CRC=%02x (ok)", packet->crc.crc8);
	else
		fprintf(stderr, "CRC=%04x (ok)", packet->crc.crc16);
	
	fprintf(stderr, "\n");
}

void update_summary(const bool show_retransmits, const bool is_retransmit) //add stuff?
{
	static uint64_t nb_valid_packets=0;
	static uint64_t nb_retransmits=0;
	
	nb_valid_packets++;
	if(is_retransmit)
		nb_retransmits++;
	
	if(show_retransmits)
		fprintf(stderr, "nRF24 %lu packets, %lu retransmits\r", nb_valid_packets, nb_retransmits);
	else
		fprintf(stderr, "nRF24 %lu packets\r", nb_valid_packets);
}

bool make_packet_from_samples(uint32_t startpos_samples, nRF24_packet_t * const packet, payloadlengthmode_t mode, const uint8_t sz_payload, uint16_t * const packetsize_samples)
{
	read_bytes(startpos_samples, sz_addr_bytes, packet->addr);
	startpos_samples+=BYTES_TO_SAMPLES(sz_addr_bytes);
	uint32_t sz_samples=BYTES_TO_SAMPLES(sz_addr_bytes);
	
	if(nrfmode==MODE_NORMAL)
	{
		read_pcf(startpos_samples, packet);
		startpos_samples+=BITS_TO_SAMPLES(BITS_PCF);	
		sz_samples+=BITS_TO_SAMPLES(BITS_PCF);	
	}
	
	if(mode==PAYLOAD_FIXED_LENGTH)
	{		
		read_bytes(startpos_samples, sz_payload, packet->payload);
		packet->sz_payload_bytes=sz_payload;
		startpos_samples+=BYTES_TO_SAMPLES(sz_payload);
		sz_samples+=BYTES_TO_SAMPLES(sz_payload);
	}
	else
	{
		if(packet->pcf.payload_length>32)
			return false; //this can't be a valid packet
		
		read_bytes(startpos_samples, packet->pcf.payload_length, packet->payload);
		packet->sz_payload_bytes=packet->pcf.payload_length;
		startpos_samples+=BYTES_TO_SAMPLES(packet->pcf.payload_length);
		sz_samples+=BYTES_TO_SAMPLES(packet->pcf.payload_length);
	}
	
	if(crcmode==CRC_ONE_BYTE)
	{
		packet->crc.crc8=get_byte(startpos_samples);
		startpos_samples+=BYTES_TO_SAMPLES(1);
		sz_samples+=BYTES_TO_SAMPLES(1);
	}
	else
	{
		packet->crc.crc16=((uint16_t)get_byte(startpos_samples))<<8|get_byte(startpos_samples+BYTES_TO_SAMPLES(1));
		startpos_samples+=BYTES_TO_SAMPLES(2);
		sz_samples+=BYTES_TO_SAMPLES(2);
	}
	
	(*packetsize_samples)=sz_samples;
	
	return true;
}

bool check_display_packet(uint16_t * const packetsize_samples)
{
	nRF24_packet_t packet;
	
	uint32_t startpos_samples=BITS_TO_SAMPLES(BITS_PREAMBLE);
	
	uint8_t buf[BUF_CRC_MAX];
	static uint8_t buf_previous[BUF_CRC_MAX];

	uint16_t bits_total;
	static uint16_t bits_total_previous=0;
	
	bool is_retransmit;
	
	struct timeval tv;
	
	uint8_t i;
	
	if(payloadlengthmode==PAYLOAD_DYNAMIC_LENGTH || sz_payload_bytes==sz_ack_payload_bytes) //there is no way to distinguish between data-packets and ack-packets with payload
	{
		if(payloadlengthmode==PAYLOAD_DYNAMIC_LENGTH)
		{
			if(!make_packet_from_samples(startpos_samples, &packet, PAYLOAD_DYNAMIC_LENGTH, 0, packetsize_samples))
				return false; //invalid packet
			bits_total=pack_for_crc(buf, &packet, packet.pcf.payload_length);
		}
		else
		{
			make_packet_from_samples(startpos_samples, &packet, PAYLOAD_FIXED_LENGTH, sz_payload_bytes, packetsize_samples); //or sz_payload_ack_bytes, they have the same value
			bits_total=pack_for_crc(buf, &packet, sz_payload_bytes); //or sz_payload_ack_bytes, they have the same value
		}
		if((crcmode==CRC_ONE_BYTE && calc_crc8(buf, bits_total)==packet.crc.crc8) || calc_crc16(buf, bits_total)==packet.crc.crc16)
		{
			if(filtermode==FILTER_BY_ADDRESS && memcmp(packet.addr, filter_by_address, sz_addr_bytes))
				return true; //valid packet but nothing to be displayed because the address does not match
			
			if(dispmode==DISP_VERBOSE)
			{
				gettimeofday(&tv, NULL);
				disp_packet_verbose(&packet, &tv, PACKET_UNDISTINGUISHABLE, false);
			}
			else if(dispmode==DISP_SUMMARY)
				update_summary(false, false);
			
			if(dumpmode==DUMP_PACKET_AND_ACK_PAYLOAD)
				for(i=0; i<packet.sz_payload_bytes; i++)
					putc(packet.payload[i], stdout);
			
			
			return true;
		}
		else //no valid packet, CRC does not match
			return false;
	}
	else //we have a way to distinguish between data-packets and ack-packets with or without payload using CRC
	{
		packettype_t packettype=PACKET_INVALID;
		
		//is data-packet?
		make_packet_from_samples(startpos_samples, &packet, PAYLOAD_FIXED_LENGTH, sz_payload_bytes, packetsize_samples);
		bits_total=pack_for_crc(buf, &packet, packet.sz_payload_bytes);
		if((crcmode==CRC_ONE_BYTE && calc_crc8(buf, bits_total)==packet.crc.crc8) || calc_crc16(buf, bits_total)==packet.crc.crc16)
			packettype=PACKET_DATA_PACKET;
		else //is ack-packet?
		{
			make_packet_from_samples(startpos_samples, &packet, PAYLOAD_FIXED_LENGTH, sz_ack_payload_bytes, packetsize_samples);
			bits_total=pack_for_crc(buf, &packet, packet.sz_payload_bytes);
			if((crcmode==CRC_ONE_BYTE && calc_crc8(buf, bits_total)==packet.crc.crc8) || calc_crc16(buf, bits_total)==packet.crc.crc16)
				packettype=PACKET_ACK_PACKET;
			else
				return false; //invalid packet, no CRC-match
		}
		
		if(filtermode==FILTER_BY_ADDRESS && memcmp(packet.addr, filter_by_address, sz_addr_bytes))
			return true; //valid packet but nothing to be displayed because the address does not match
		
		if(packettype==PACKET_DATA_PACKET)
		{
			if(nrfmode==MODE_NORMAL && bits_total==bits_total_previous && !memcmp(buf, buf_previous, (bits_total+4)/8))
				is_retransmit=true;
			else
			{
				is_retransmit=false;
				bits_total_previous=bits_total;
				memcpy(buf_previous, buf, (bits_total+4)/8);
			}
			
			if(dispmode==DISP_VERBOSE || (dispmode==DISP_RETRANSMITS_ONLY && is_retransmit))
			{
				gettimeofday(&tv, NULL);
				disp_packet_verbose(&packet, &tv, PACKET_DATA_PACKET, is_retransmit);
			}
			else if(dispmode==DISP_SUMMARY)
				update_summary(true, is_retransmit);
			
			if(dumpmode==DUMP_PACKET_PAYLOAD || dumpmode==DUMP_PACKET_AND_ACK_PAYLOAD)
				for(i=0; i<packet.sz_payload_bytes; i++)
					putc(packet.payload[i], stdout);
		}
		else //PACKET_ACK_PACKET
		{
			if(dispmode==DISP_VERBOSE)
			{
				gettimeofday(&tv, NULL);
				disp_packet_verbose(&packet, &tv, PACKET_ACK_PACKET, false);
			}
			else if(dispmode==DISP_SUMMARY)
				update_summary(true, false);
			
			if(dumpmode==DUMP_ACK_PAYLOAD || dumpmode==DUMP_PACKET_AND_ACK_PAYLOAD)
				for(i=0; i<packet.sz_payload_bytes; i++)
					putc(packet.payload[i], stdout);
		}
		
		return true;
	}
}

void print_usage_and_exit(void)
{
	fprintf(stderr, "usage: cat $pipe_or_file | ./nrf-decoder [options]\n");
	fprintf(stderr, "options:\n\t--spb $samples_per_bit (mandatory)\n\t--sz-addr $sz_addr_bytes (mandatory)\n\t--sz-payload $sz_payload_bytes\n\t--sz-ack-payload $sz_ack_payload_bytes\n\t--dyn-lengths\n\t--disp [verbose|retransmits|none]\n\t--dump-payload [data|ack|all]\n\t--mode-compatibility\n\t--crc16\n\t--filter-addr $addr_in_hex\n");
	exit(0);
}

void parse_dispmode(char const * const str)
{
	if(!strcmp(str, "verbose"))
		dispmode=DISP_VERBOSE;
	else if(!strcmp(str, "retransmits"))
		dispmode=DISP_RETRANSMITS_ONLY;
	else if(!strcmp(str, "none"))
		dispmode=DISP_NONE;
	else
		errx(1, "invalid argument for --disp");
}

void parse_dumpmode(char const * const str)
{
	if(!strcmp(str, "data"))
		dumpmode=DUMP_PACKET_PAYLOAD;
	else if(!strcmp(str, "ack"))
		dumpmode=DUMP_ACK_PAYLOAD;
	else if(!strcmp(str, "all"))
		dumpmode=DUMP_PACKET_AND_ACK_PAYLOAD;
	else
		errx(1, "invalid argument for --dump-payload");
}

uint8_t parse_hex_byte(char const * const str)
{
	uint8_t ret;
	
	if(isdigit(str[0]))
		ret=(str[0]-'0')<<4;
	else
		ret=(tolower(str[0])-'a'+10)<<4;
	
	if(isdigit(str[1]))
		ret|=(str[1]-'0');
	else
		ret|=(tolower(str[1])-'a'+10);
	
	return ret;
}

void parse_filter_addr(char const * const str, uint8_t * const sz_parsed_addr)
{
	char const * ptr=str;
	if(!memcmp(ptr,"0x",2))
		ptr+=2;
	
	uint8_t len=strlen(ptr);
	
	if(len%2)
		errx(1, "invalid argument for --filter-address: use always 2 hex-characters per byte");
	
	uint8_t i,j;
	
	(*sz_parsed_addr)=0;
	
	for(i=0,j=0; (i<len && j<5); i+=2,j++)
	{
		if(!isxdigit(ptr[i]) || !isxdigit(ptr[i+1]))
			errx(1, "invalid argument for --filter-address: invalid character found");
		filter_by_address[j]=parse_hex_byte(&ptr[i]);
		(*sz_parsed_addr)++;
	}
}

static volatile bool run=true;

static void sigint(int sig)
{
	(void)sig;
	run=false;
}

int main(int argc, char **argv)
{
	const struct option optiontable[]=
	{
		{ "spb",				required_argument,	NULL,	0 },
		{ "sz-addr",	 		required_argument,	NULL,	1 },
		{ "sz-payload",	 		required_argument,	NULL,	2 },
		{ "sz-ack-payload",	 	required_argument,	NULL,	3 },
		{ "dyn-lengths",		no_argument,		NULL,	4 },
		{ "mode-compatibility",	no_argument,		NULL,	5 },
		{ "crc16",				no_argument,		NULL,	6 },
		{ "disp",				required_argument,	NULL,	7 },
		{ "dump-payload",		required_argument,	NULL,	8 },
		{ "filter-addr",		required_argument,	NULL,	9 },
		
		{ "version",			no_argument,		NULL, 	100 },
		{ "help",				no_argument,		NULL, 	101 },
		{ "usage",				no_argument,		NULL, 	101 },
		
		{ NULL, 0, NULL, 0 }
	};

	int optionindex;
	int opt;
	
	uint8_t sz_parsed_addr;
	
	bool only_print_version=false;
	
	fprintf(stderr, "This is nrf-decoder version 1 (c) 2022 by kittennbfive.\n");
	fprintf(stderr, "This tool is experimental and provided under AGPLv3+ WITHOUT ANY WARRANTY!\n\n");
	
	while((opt=getopt_long(argc, argv, "", optiontable, &optionindex))!=-1)
	{
		switch(opt)
		{
			case '?': print_usage_and_exit(); break;
			case 0: samples_per_bit=atoi(optarg); break;
			case 1: sz_addr_bytes=atoi(optarg); break;
			case 2: sz_payload_bytes=atoi(optarg); break;
			case 3: sz_ack_payload_bytes=atoi(optarg); sz_ack_payload_bytes_specified=true; break;
			case 4: payloadlengthmode=PAYLOAD_DYNAMIC_LENGTH; break;
			case 5: nrfmode=MODE_COMPATIBILITY; break;
			case 6: crcmode=CRC_TWO_BYTES; break;
			case 7: parse_dispmode(optarg); break;
			case 8: parse_dumpmode(optarg); break;
			case 9: filtermode=FILTER_BY_ADDRESS; parse_filter_addr(optarg, &sz_parsed_addr); break;
			
			case 100: only_print_version=true; break;
			case 101: print_usage_and_exit(); break;
			
			default: errx(1, "don't know how to handle %d returned by getopt_long", opt); break;
		}
	}
	
	if(only_print_version)
		return 0;
	
	if(samples_per_bit==0)
		errx(1, "invalid value for or missing mandatory argument --spb");
	
	if(sz_addr_bytes==0)
		errx(1, "invalid value for or missing mandatory argument --sz-addr");
	
	if(sz_payload_bytes==0 && payloadlengthmode==PAYLOAD_FIXED_LENGTH)
		errx(1, "invalid value for or missing mandatory argument --sz-payload if --dyn-lengths is not specified");

	if(!sz_ack_payload_bytes_specified && payloadlengthmode==PAYLOAD_FIXED_LENGTH && nrfmode==MODE_NORMAL)
		errx(1, "invalid value for or missing mandatory argument --sz-ack-payload if --dyn-lengths is not specified in normal mode");

	if(payloadlengthmode==PAYLOAD_DYNAMIC_LENGTH && sz_payload_bytes!=0)
		warnx("--dyn-payload-length is set, ignoring --sz-payload\n");

	if(payloadlengthmode==PAYLOAD_DYNAMIC_LENGTH && sz_ack_payload_bytes!=0)
		warnx("--dyn-payload-length is set, ignoring --sz-ack-payload\n");
	
	if(filtermode==FILTER_BY_ADDRESS && sz_addr_bytes!=sz_parsed_addr)
		errx(1, "size missmatch between specified address length and specified address for filtering");
	
	if((dumpmode==DUMP_PACKET_AND_ACK_PAYLOAD || dumpmode==DUMP_ACK_PAYLOAD) && nrfmode==MODE_COMPATIBILITY)
		errx(1, "--dump-payload [ack|all] is incompatible with --mode-compatibility (ACK-packets can't have payload in this mode)");

	if((dumpmode==DUMP_PACKET_PAYLOAD || dumpmode==DUMP_ACK_PAYLOAD) && (payloadlengthmode==PAYLOAD_DYNAMIC_LENGTH || sz_payload_bytes==sz_ack_payload_bytes))
		errx(1, "--dump-payload [data|ack] can't be used when --sz-payload equals --sz-ack-payload or --dyn-lengths is used because there is no way to distinguish between data-packets and ACK-packets");

	if(dispmode==DISP_RETRANSMITS_ONLY && (payloadlengthmode==PAYLOAD_DYNAMIC_LENGTH || sz_payload_bytes==sz_ack_payload_bytes))
		errx(1, "--disp retransmits will not work with --dyn-lengths or if --sz-payload equals --sz-ack-payload");

	ringbuffer=malloc(SZ_BUFFER_SAMPLES);
	if(!ringbuffer)
		err(1, "malloc for ring buffer failed");
	
	signal(SIGINT, &sigint);
	
	uint16_t packetsize_samples;

	while(!feof(stdin) && run)
	{
		ringbuffer_put_sample(fgetc(stdin));
		
		if(nb_samples>=MAX_PACKET_LENGTH_SAMPLES)
		{
			if(check_for_preamble())
			{
				if(check_display_packet(&packetsize_samples))
					ringbuffer_remove_samples(packetsize_samples);
				else
					ringbuffer_remove_samples(1);
			}
			else
				ringbuffer_remove_samples(1);
		}
	}
	
	if(dispmode==DISP_SUMMARY) //to avoid summary being overwritten by shell
		fprintf(stderr, "\n");

	free(ringbuffer);
	
	fprintf(stderr, "\nall done, bye\n");
	
	return 0;
}
