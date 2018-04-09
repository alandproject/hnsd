#include "config.h"

#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "addr.h"
#include "constants.h"
#include "dns.h"
#include "ec.h"
#include "error.h"
#include "req.h"
#include "sig0.h"
#include "utils.h"

void
hsk_dns_req_init(hsk_dns_req_t *req) {
  assert(req);
  req->ns = NULL;
  req->id = 0;
  req->labels = 0;
  memset(req->name, 0x00, sizeof(req->name));
  req->type = 0;
  req->class = 0;
  req->edns = false;
  req->dnssec = false;
  memset(req->tld, 0x00, sizeof(req->tld));
  memset(&req->ss, 0x00, sizeof(struct sockaddr_storage));
  req->addr = (struct sockaddr *)&req->ss;
}

void
hsk_dns_req_uninit(hsk_dns_req_t *req) {
  assert(req);
}

hsk_dns_req_t *
hsk_dns_req_alloc(void) {
  hsk_dns_req_t *req = malloc(sizeof(hsk_dns_req_t));
  if (req)
    hsk_dns_req_init(req);
  return req;
}

void
hsk_dns_req_free(hsk_dns_req_t *req) {
  assert(req);
  hsk_dns_req_uninit(req);
  free(req);
}

hsk_dns_req_t *
hsk_dns_req_create(uint8_t *data, size_t data_len, struct sockaddr *addr) {
  hsk_dns_req_t *req = NULL;
  hsk_dns_msg_t *msg = NULL;

  req = hsk_dns_req_alloc();

  if (!req)
    goto fail;

  if (!hsk_dns_msg_decode(data, data_len, &msg))
    goto fail;

  if (msg->opcode != HSK_DNS_QUERY
      || msg->code != HSK_DNS_NOERROR
      || msg->qd.size != 1
      || msg->an.size != 0
      || msg->ns.size != 0) {
    goto fail;
  }

  // Grab the first question.
  hsk_dns_qs_t *qs = msg->qd.items[0];

  if (qs->class != HSK_DNS_IN)
    goto fail;

  // Don't allow dirty names.
  if (hsk_dns_name_dirty(qs->name))
    goto fail;

  // Check for a TLD.
  hsk_dns_label_get(qs->name, -1, req->tld);

  // Lowercase.
  hsk_to_lower(req->tld);

  // Reference.
  req->ns = NULL;

  // DNS stuff.
  req->id = msg->id;
  req->labels = hsk_dns_label_count(qs->name);
  strcpy(req->name, qs->name);
  req->type = qs->type;
  req->class = qs->class;
  req->rd = (msg->flags & HSK_DNS_RD) != 0;
  req->cd = (msg->flags & HSK_DNS_CD) != 0;
  req->edns = msg->edns.enabled;
  req->max_size = HSK_DNS_MAX_UDP;
  if (msg->edns.enabled && msg->edns.size >= HSK_DNS_MAX_UDP)
    req->max_size = msg->edns.size;
  req->dnssec = (msg->edns.flags & HSK_DNS_DO) != 0;

  // Sender address.
  hsk_sa_copy(req->addr, addr);

  // Free stuff up.
  hsk_dns_msg_free(msg);

  return req;

fail:
  if (req)
    free(req);

  if (msg)
    hsk_dns_msg_free(msg);

  return NULL;
}

void
hsk_dns_req_print(hsk_dns_req_t *req, char *prefix) {
  assert(req);

  if (!prefix)
    prefix = "";

  char addr[HSK_MAX_HOST];

  assert(hsk_sa_to_string(req->addr, addr, HSK_MAX_HOST, 1));

  printf("%squery\n", prefix);
  printf("%s  id=%d\n", prefix, req->id);
  printf("%s  labels=%d\n", prefix, req->labels);
  printf("%s  name=%s\n", prefix, req->name);
  printf("%s  type=%d\n", prefix, req->type);
  printf("%s  class=%d\n", prefix, req->class);
  printf("%s  edns=%d\n", prefix, (int32_t)req->edns);
  printf("%s  dnssec=%d\n", prefix, (int32_t)req->dnssec);
  printf("%s  tld=%s\n", prefix, req->tld);
  printf("%s  addr=%s\n", prefix, addr);
}

bool
hsk_dns_msg_finalize(
  hsk_dns_msg_t **res,
  hsk_dns_req_t *req,
  hsk_ec_t *ec,
  uint8_t *key,
  uint8_t **wire,
  size_t *wire_len
) {
  assert(res && req && ec && wire && wire_len);

  hsk_dns_msg_t *msg = *res;

  *res = NULL;
  *wire = NULL;
  *wire_len = 0;

  // Reset ID & flags.
  msg->id = req->id;
  msg->flags |= HSK_DNS_QR;

  if (req->rd)
    msg->flags |= HSK_DNS_RD;

  if (req->cd)
    msg->flags |= HSK_DNS_CD;

  // Reset EDNS stuff.
  msg->edns.enabled = false;
  msg->edns.version = 0;
  msg->edns.flags = 0;
  msg->edns.size = 512;
  msg->edns.code = 0;
  msg->edns.rd_len = 0;

  if (msg->edns.rd) {
    free(msg->edns.rd);
    msg->edns.rd = NULL;
  }

  if (req->edns) {
    msg->edns.enabled = true;
    msg->edns.size = 4096;
    if (req->dnssec)
      msg->edns.flags |= HSK_DNS_DO;
  }

  // Reset question.
  hsk_dns_rrs_uninit(&msg->qd);

  hsk_dns_qs_t *qs = hsk_dns_qs_alloc();

  if (!qs) {
    hsk_dns_msg_free(msg);
    return NULL;
  }

  qs->type = req->type;
  hsk_dns_rr_set_name(qs, req->name);

  hsk_dns_rrs_push(&msg->qd, qs);

  // Remove RRSIGs if they didn't ask for them.
  if (!req->dnssec) {
    if (!hsk_dns_msg_clean(msg, req->type)) {
      hsk_dns_msg_free(msg);
      return false;
    }
  }

  // Reserialize.
  uint8_t *data = NULL;
  size_t data_len = 0;

  if (!hsk_dns_msg_encode(msg, &data, &data_len)) {
    hsk_dns_msg_free(msg);
    return false;
  }

  assert(data);

  hsk_dns_msg_free(msg);

  // Truncate.
  size_t max = req->max_size;

  if (key)
    max -= HSK_SIG0_RR_SIZE;

  if (!hsk_dns_msg_truncate(data, data_len, max, &data_len)) {
    free(data);
    return false;
  }

  // Sign.
  uint8_t *out = NULL;
  size_t out_len = 0;

  if (!hsk_sig0_sign(ec, key, data, data_len, &out, &out_len)) {
    free(data);
    return false;
  }

  assert(out);
  free(data);

  *wire = out;
  *wire_len = out_len;

  return true;
}
