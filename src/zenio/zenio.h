#ifndef ZENIO_ZENIO_H
#define ZENIO_ZENIO_H

// ZENIO 的总入口: 使用者只包含这个头文件 (S22)。

#include "zenio/cancel.h"
#include "zenio/error.h"
#include "zenio/join_set.h"
#include "zenio/net/address.h"
#include "zenio/net/tcp.h"
#include "zenio/net/udp.h"
#include "zenio/result.h"
#include "zenio/runtime.h"
#include "zenio/select.h"
#include "zenio/signal.h"
#include "zenio/spawn.h"
#include "zenio/task.h"
#include "zenio/time.h"
#include "zenio/try.h"
#include "zenio/version.h"

#endif    // ZENIO_ZENIO_H
