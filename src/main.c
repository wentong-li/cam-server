/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/** @file
 * @brief WiFi shell sample main function
 */

#include <zephyr/sys/printk.h>
#include <zephyr/kernel.h>
#if defined(CLOCK_FEATURE_HFCLK_DIVIDE_PRESENT) || NRF_CLOCK_HAS_HFCLK192M
#include <nrfx_clock.h>
#endif
#include <zephyr/device.h>
#include <zephyr/net/net_config.h>
#include <stdio.h>

#if !defined(__ZEPHYR__) || defined(CONFIG_POSIX_API)

#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

#else

#include <zephyr/net/socket.h>
#include <zephyr/kernel.h>

#include <zephyr/net/net_pkt.h>

#endif

#define BIND_PORT 8080

#include <zephyr/drivers/video.h>
#include <zephyr/drivers/video/arducam_mega.h>

#define CHECK(r) { if (r == -1) { printf("Error: " #r "\n"); exit(1); } }

const struct device *video;
struct video_buffer *vbuf;
static struct arducam_mega_info mega_info;

const char JHEADER[] = "HTTP/1.1 200 OK\r\n" \
                       "Content-disposition: inline; filename=capture.jpg\r\n" \
                       "Content-type: image/jpeg\r\n\r\n";

const char HEADER[] = "HTTP/1.1 200 OK\r\n" \
                      "Access-Control-Allow-Origin: *\r\n" \
                      "Content-Type: multipart/x-mixed-replace; boundary=123456789000000000000987654321\r\n";
const char BOUNDARY[] = "\r\n--123456789000000000000987654321\r\n";
const char CTNTTYPE[] = "Content-Type: image/jpeg\r\nContent-Length: ";


uint8_t img_buffer[40000];
uint32_t img_buffer_size;
void get_image(void) {
	uint32_t timestamp;
	uint32_t cursor;


	video_dequeue(video, VIDEO_EP_OUT, &vbuf, K_FOREVER);
	//mark down the current timestamp, and start recording when next frame comes
	timestamp = vbuf->timestamp;
	img_buffer_size = vbuf->bytesframe;
	cursor = 0;
	memcpy(img_buffer+cursor, vbuf->buffer, vbuf->bytesused);
	cursor += vbuf->bytesused;
	video_enqueue(video, VIDEO_EP_OUT, vbuf);
	while (cursor < img_buffer_size) {
		video_dequeue(video, VIDEO_EP_OUT, &vbuf, K_FOREVER);
		memcpy(img_buffer+cursor, vbuf->buffer, vbuf->bytesused);
		cursor += vbuf->bytesused;
		video_enqueue(video, VIDEO_EP_OUT, vbuf);
	}
}

int main(void)
{

#if defined(CLOCK_FEATURE_HFCLK_DIVIDE_PRESENT) || NRF_CLOCK_HAS_HFCLK192M
	/* For now hardcode to 128MHz */
	nrfx_clock_divider_set(NRF_CLOCK_DOMAIN_HFCLK,
			       NRF_CLOCK_HFCLK_DIV_1);
#endif
	printk("Starting %s with CPU frequency: %d MHz\n", CONFIG_BOARD, SystemCoreClock/MHZ(1));



	int ret;
	struct video_buffer *buffers[3];

	video = DEVICE_DT_GET(DT_NODELABEL(arducam_mega0));

	if (!device_is_ready(video))
	{
		printk("Video device %s not ready.", video->name);
		return -1;
	}

	video_stream_stop(video);
	printk("Device %s is ready!", video->name);
	k_msleep(100);
	video_get_ctrl(video, VIDEO_CID_ARDUCAM_INFO, &mega_info);
	struct video_format format;
	format.height = 480;
	format.width = 640;
	format.pixelformat = VIDEO_PIX_FMT_JPEG;
	video_set_format(video, VIDEO_EP_ANY, &format);
	for (int i = 0; i < ARRAY_SIZE(buffers); i++)
	{
		buffers[i] = video_buffer_alloc(4096);
		if (buffers[i] == NULL)
		{
			printk("Unable to alloc video buffer");
			return -1;
		}
		video_enqueue(video, VIDEO_EP_OUT, buffers[i]);
	}
	video_stream_start(video);



	int serv;
	struct sockaddr_in bind_addr;
	static int counter;

	serv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	CHECK(serv);

	bind_addr.sin_family = AF_INET;
	bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	bind_addr.sin_port = htons(BIND_PORT);
	CHECK(bind(serv, (struct sockaddr *)&bind_addr, sizeof(bind_addr)));

	CHECK(listen(serv, 5));

	printf("Single-threaded dumb HTTP server waits for a connection on "
	       "port %d...\n", BIND_PORT);

	while (1) {
		struct sockaddr_in client_addr;
		socklen_t client_addr_len = sizeof(client_addr);
		char addr_str[32];
		int req_state = 0;
		const char *data;
		size_t len;

		int client = accept(serv, (struct sockaddr *)&client_addr,
				    &client_addr_len);
		if (client < 0) {
			printf("Error in accept: %d - continuing\n", errno);
			continue;
		}

		inet_ntop(client_addr.sin_family, &client_addr.sin_addr,
			  addr_str, sizeof(addr_str));
		printf("Connection #%d from %s\n", counter++, addr_str);

		/* Discard HTTP request (or otherwise client will get
		 * connection reset error).
		 */
		while (1) {
			ssize_t r;
			char c;

			r = recv(client, &c, 1, 0);
			if (r == 0) {
				goto close_client;
			}

			if (r < 0) {
				if (errno == EAGAIN || errno == EINTR) {
					continue;
				}

				printf("Got error %d when receiving from "
				       "socket\n", errno);
				goto close_client;
			}
			if (req_state == 0 && c == '\r') {
				req_state++;
			} else if (req_state == 1 && c == '\n') {
				req_state++;
			} else if (req_state == 2 && c == '\r') {
				req_state++;
			} else if (req_state == 3 && c == '\n') {
				break;
			} else {
				req_state = 0;
			}
		}
		/* HTTP Header */
		data = HEADER;
		len = strlen(HEADER);
		while (len) {
			int sent_len = send(client, data, len, 0);

			if (sent_len == -1) {
				printf("Error sending data to peer, errno: %d\n", errno);
				break;
			}
			data += sent_len;
			len -= sent_len;
		}
		/* HTTP boundary */
		data = BOUNDARY;
		len = strlen(BOUNDARY);
		while (len) {
			int sent_len = send(client, data, len, 0);

			if (sent_len == -1) {
				printf("Error sending data to peer, errno: %d\n", errno);
				break;
			}
			data += sent_len;
			len -= sent_len;
		}

		/* Keep sending images */
		uint64_t t_start, t_end;
		while (1) {

			get_image();

			t_start = k_uptime_get();


			/* HTTP content type */
			data = CTNTTYPE;
			len = strlen(CTNTTYPE);
			while (len) {
				int sent_len = send(client, data, len, 0);

				if (sent_len == -1) {
					printf("Error sending data to peer, errno: %d\n", errno);
					goto close_client;
				}
				data += sent_len;
				len -= sent_len;
			}
			/* Image size */
			char buf[20];
			sprintf(buf, "%d\r\n\r\n", img_buffer_size);

			data = buf;
			len = strlen(buf);
			while (len) {
				int sent_len = send(client, data, len, 0);

				if (sent_len == -1) {
					printf("Error sending data to peer, errno: %d\n", errno);
					goto close_client;
				}
				data += sent_len;
				len -= sent_len;
			}
			/* message body containing img bytes */
			data = img_buffer;
			len = img_buffer_size;
			while (len) {
				int sent_len = send(client, data, len, 0);

				if (sent_len == -1) {
					printf("Error sending data to peer, errno: %d\n", errno);
					goto close_client;
				}
				data += sent_len;
				len -= sent_len;
			}
			/* HTTP boundary */
			data = BOUNDARY;
			len = strlen(BOUNDARY);
			while (len) {
				int sent_len = send(client, data, len, 0);

				if (sent_len == -1) {
					printf("Error sending data to peer, errno: %d\n", errno);
					goto close_client;
				}
				data += sent_len;
				len -= sent_len;
			}
			t_end = k_uptime_get();
			printk("Transmission %lld ms.\n", t_end - t_start);
			k_msleep(10);

		}


close_client:
		ret = close(client);
		if (ret == 0) {
			printf("Connection from %s closed\n", addr_str);
		} else {
			printf("Got error %d while closing the "
			       "socket\n", errno);
		}

#if defined(__ZEPHYR__) && defined(CONFIG_NET_BUF_POOL_USAGE)
		struct k_mem_slab *rx, *tx;
		struct net_buf_pool *rx_data, *tx_data;

		net_pkt_get_info(&rx, &tx, &rx_data, &tx_data);
		printf("rx buf: %d, tx buf: %d\n",
		       atomic_get(&rx_data->avail_count), atomic_get(&tx_data->avail_count));
#endif

	}

	return 0;
}
