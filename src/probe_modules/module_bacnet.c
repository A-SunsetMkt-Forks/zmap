/*
 * ZMap Copyright 2013 Regents of the University of Michigan
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not
 * use this file except in compliance with the License. You may obtain a copy
 * of the License at http://www.apache.org/licenses/LICENSE-2.0
 */

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>

#include "../../lib/includes.h"
#include "packet.h"
#include "probe_modules.h"
#include "module_bacnet.h"
#include "module_udp.h"
#include "logger.h"

#define ICMP_UNREACH_HEADER_SIZE 8

#define SOURCE_PORT_VALIDATION_MODULE_DEFAULT true; // default to validating source port
static bool should_validate_src_port = SOURCE_PORT_VALIDATION_MODULE_DEFAULT

#define ZMAP_BACNET_PACKET_LEN                             \
	(sizeof(struct ether_header) + sizeof(struct ip) + \
	 sizeof(struct udphdr) + 0x11)

probe_module_t module_bacnet;

static int num_ports;

static uint8_t bacnet_body[] = {0x0c, 0x02, 0x3f, 0xff, 0xff, 0x19, 0x4b};
#define BACNET_BODY_LEN 7

static inline uint8_t get_invoke_id(uint32_t *validation)
{
	return (uint8_t)((validation[1] >> 24) & 0xFF);
}

int bacnet_init_perthread(void **arg)
{
	uint32_t seed = aesrand_getword(zconf.aes);
	aesrand_t *aes = aesrand_init_from_seed(seed);
	*arg = aes;

	return EXIT_SUCCESS;
}

int bacnet_prepare_packet(void *buf, macaddr_t *src, macaddr_t *gw, UNUSED void *arg)
{
	memset(buf, 0, MAX_PACKET_SIZE);
	struct ether_header *eth_header = (struct ether_header *)buf;
	struct ip *ip_header = (struct ip *)(&eth_header[1]);
	struct udphdr *udp_header = (struct udphdr *)(&ip_header[1]);
	struct bacnet_probe *bnp = (struct bacnet_probe *)&udp_header[1];
	uint8_t *body = (uint8_t *)&bnp[1];

	make_eth_header(eth_header, src, gw);

	uint16_t ip_len = sizeof(struct ip) + sizeof(struct udphdr) + 0x11;
	assert(ip_len <= MAX_PACKET_SIZE);
	make_ip_header(ip_header, IPPROTO_UDP, htons(ip_len));

	uint16_t udp_len = sizeof(struct udphdr) + 0x11;
	make_udp_header(udp_header, udp_len);

	bnp->vlc.type = ZMAP_BACNET_TYPE_IP;
	bnp->vlc.function = ZMAP_BACNET_FUNCTION_UNICAST_NPDU;
	bnp->vlc.length = htons(0x11);

	bnp->npdu.version = ZMAP_BACNET_NPDU_VERSION_ASHRAE_135_1995;
	bnp->npdu.control = 0x04;

	bnp->apdu.type_flags = 0x00;
	bnp->apdu.max_segments_apdu = 0x05;
	bnp->apdu.server_choice = 0x0c;
	memcpy(body, bacnet_body, BACNET_BODY_LEN);

	return EXIT_SUCCESS;
}

int bacnet_make_packet(void *buf, size_t *buf_len, ipaddr_n_t src_ip,
		       ipaddr_n_t dst_ip, port_n_t dport, uint8_t ttl,
		       uint32_t *validation, int probe_num, uint16_t ip_id,
		       UNUSED void *arg)
{
	struct ether_header *eth_header = (struct ether_header *)buf;
	struct ip *ip_header = (struct ip *)(&eth_header[1]);
	struct udphdr *udp_header = (struct udphdr *)&ip_header[1];
	struct bacnet_probe *bnp = (struct bacnet_probe *)&udp_header[1];

	ip_header->ip_src.s_addr = src_ip;
	ip_header->ip_dst.s_addr = dst_ip;
	ip_header->ip_ttl = ttl;
	ip_header->ip_sum = 0;
	ip_header->ip_id = ip_id;

	udp_header->uh_sport =
	    htons(get_src_port(num_ports, probe_num, validation));
	udp_header->uh_dport = dport;

	bnp->apdu.invoke_id = get_invoke_id(validation);

	ip_header->ip_sum = zmap_ip_checksum((unsigned short *)ip_header);
	*buf_len = ZMAP_BACNET_PACKET_LEN;

	return EXIT_SUCCESS;
}

int bacnet_validate_packet(const struct ip *ip_hdr, uint32_t len,
			   uint32_t *src_ip, uint32_t *validation,
			   const struct port_conf *ports)
{
	// this will reject packets that aren't UDP or ICMP and fully process ICMP packets
	if (udp_do_validate_packet(ip_hdr, len, src_ip, validation, num_ports, should_validate_src_port, ports) == PACKET_INVALID) {
		return PACKET_INVALID;
	}
	if (ip_hdr->ip_p == IPPROTO_UDP) {
		struct udphdr *udp = get_udp_header(ip_hdr, len);
		if (!udp) {
			return PACKET_INVALID;
		}
		const size_t min_len =
		    sizeof(struct udphdr) + sizeof(struct bacnet_vlc);
		if (udp->uh_ulen < min_len) {
			return PACKET_INVALID;
		}
		struct bacnet_vlc *vlc =
		    (struct bacnet_vlc *)get_udp_payload(udp, len);
		if (vlc->type != ZMAP_BACNET_TYPE_IP) {
			return PACKET_INVALID;
		}
	}
	return PACKET_VALID;
}

void bacnet_process_packet(const u_char *packet, uint32_t len, fieldset_t *fs,
			   UNUSED uint32_t *validation,
			   UNUSED struct timespec ts)
{
	struct ip *ip_hdr = get_ip_header(packet, len);
	assert(ip_hdr);
	if (ip_hdr->ip_p == IPPROTO_UDP) {
		struct udphdr *udp = get_udp_header(ip_hdr, len);
		assert(udp);
		fs_add_uint64(fs, "sport", ntohs(udp->uh_sport));
		fs_add_uint64(fs, "dport", ntohs(udp->uh_dport));
		fs_add_constchar(fs, "classification", "bacnet");
		fs_add_bool(fs, "success", 1);
		fs_add_null_icmp(fs);
		uint32_t udp_offset =
		    sizeof(struct ether_header) + ip_hdr->ip_hl * 4;
		uint32_t payload_offset = udp_offset + sizeof(struct udphdr);
		assert(payload_offset < len);
		uint8_t *payload = get_udp_payload(udp, len);
		uint32_t payload_len = len - payload_offset;
		fs_add_binary(fs, "udp_payload", payload_len, (void *)payload,
			      0);
		fs_add_null_icmp(fs);
	} else if (ip_hdr->ip_p == IPPROTO_ICMP) {
		fs_add_null(fs, "sport");
		fs_add_null(fs, "dport");
		fs_add_constchar(fs, "classification", "icmp");
		fs_add_bool(fs, "success", 0);
		fs_add_null(fs, "udp_payload");
		fs_populate_icmp_from_iphdr(ip_hdr, len, fs);
	}
}

int bacnet_global_initialize(struct state_conf *conf)
{
	num_ports = conf->source_port_last - conf->source_port_first + 1;
	if (conf->validate_source_port_override == VALIDATE_SRC_PORT_DISABLE_OVERRIDE) {
		log_debug("bacnet", "disabling source port validation");
		should_validate_src_port = false;
	}
	return EXIT_SUCCESS;
}

static fielddef_t fields[] = {
    {.name = "sport", .type = "int", .desc = "UDP source port"},
    {.name = "dport", .type = "int", .desc = "UDP destination port"},
    CLASSIFICATION_SUCCESS_FIELDSET_FIELDS,
    {.name = "udp_payload", .type = "binary", .desc = "UDP payload"},
    ICMP_FIELDSET_FIELDS,
};

probe_module_t module_bacnet = {.name = "bacnet",
				.max_packet_length = ZMAP_BACNET_PACKET_LEN,
				.pcap_filter = "udp || icmp",
				.pcap_snaplen = 1500,
				.port_args = 1,
				.global_initialize = &bacnet_global_initialize,
				.thread_initialize = &bacnet_init_perthread,
				.prepare_packet = &bacnet_prepare_packet,
				.make_packet = &bacnet_make_packet,
				.print_packet = &udp_print_packet,
				.validate_packet = &bacnet_validate_packet,
				.process_packet = &bacnet_process_packet,
				.close = &udp_global_cleanup,
				.output_type = OUTPUT_TYPE_STATIC,
				.fields = fields,
				.numfields =
				    sizeof(fields) / sizeof(fields[0])};
