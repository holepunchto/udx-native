const test = require('brittle')
const b4a = require('b4a')
const { Readable } = require('streamx')
const proxy = require('./helpers/proxy')
const UDX = require('../')
const { makeTwoStreams, uncaught } = require('./helpers')

test('tiny echo stream', async function (t) {
  t.plan(8)

  const [a, b] = makeTwoStreams(t)

  a.on('data', function (data) {
    t.alike(data, b4a.from('echo: hello world'), 'a received echoed data')
  })

  a.on('end', function () {
    t.pass('a ended')
  })

  a.on('finish', function () {
    t.pass('a finished')
  })

  a.on('close', function () {
    t.pass('a closed')
  })

  b.on('data', function (data) {
    t.alike(data, b4a.from('hello world'), 'b received data')
    b.write(b4a.concat([b4a.from('echo: '), data]))
  })

  b.on('end', function () {
    t.pass('b ended')
    b.end()
  })

  b.on('finish', function () {
    t.pass('b finished')
  })

  b.on('close', function () {
    t.pass('b closed')
  })

  a.write(b4a.from('hello world'))
  a.end()
})

test('stream flush', async function (t) {
  const [a, b] = makeTwoStreams(t)

  a.write('hello')
  a.write(' ')
  a.write('world')

  await a.flush()

  const all = []
  while (true) {
    const data = b.read()
    if (!data) break
    all.push(data)
  }

  const recv = b4a.concat(all)
  t.alike(recv, b4a.from('hello world'))

  a.end()
  b.end()
})

test('end immediately', async function (t) {
  t.plan(6)

  const [a, b] = makeTwoStreams(t)

  a.on('data', function () {
    t.fail('should not send data')
  })

  a.on('end', function () {
    t.pass('a ended')
  })

  a.on('finish', function () {
    t.pass('a finished')
  })

  a.on('close', function () {
    t.pass('a closed')
  })

  b.on('data', function (data) {
    t.fail('should not send data')
  })

  b.on('end', function () {
    t.pass('b ended')
    b.end()
  })

  b.on('finish', function () {
    t.pass('b finished')
  })

  b.on('close', function () {
    t.pass('b closed')
  })

  a.end()
})

test('only one side writes', async function (t) {
  t.plan(7)

  const [a, b] = makeTwoStreams(t)

  a.on('data', function () {
    t.fail('should not send data')
  })

  a.on('end', function () {
    t.pass('a ended')
  })

  a.on('finish', function () {
    t.pass('a finished')
  })

  a.on('close', function () {
    t.pass('a closed')
  })

  b.on('data', function (data) {
    t.alike(data, b4a.from('hello world'), 'b received data')
  })

  b.on('end', function () {
    t.pass('b ended')
    b.end()
  })

  b.on('finish', function () {
    t.pass('b finished')
  })

  b.on('close', function () {
    t.pass('b closed')
  })

  a.write(b4a.from('hello world'))
  a.end()
})

test('emit connect', async function (t) {
  const udx = new UDX()

  const socket = udx.createSocket()
  socket.bind(0, '127.0.0.1')

  const a = udx.createStream(1)

  a
    .on('connect', function () {
      t.pass()
      a.destroy()
      socket.close()
    })
    .connect(socket, 2, socket.address().port)
})

test('unordered messages', async function (t) {
  t.plan(2)

  const [a, b] = makeTwoStreams(t)
  const expected = []

  b.on('message', function (buf) {
    b.send(b4a.from('echo: ' + buf.toString()))
  })

  a.on('error', function () {
    t.pass('a destroyed')
  })

  a.on('message', function (buf) {
    expected.push(buf.toString())

    if (expected.length === 3) {
      t.alike(expected.sort(), [
        'echo: a',
        'echo: bc',
        'echo: d'
      ])

      // TODO: .end() here triggers a bug, investigate
      b.destroy()
    }
  })

  a.send(b4a.from('a'))
  a.send(b4a.from('bc'))
  a.send(b4a.from('d'))
})

test('try send unordered messages', async function (t) {
  t.plan(2)

  const [a, b] = makeTwoStreams(t)
  const expected = []

  b.on('message', function (buf) {
    b.trySend(b4a.from('echo: ' + buf.toString()))
  })

  a.on('error', function () {
    t.pass('a destroyed')
  })

  a.on('message', function (buf) {
    expected.push(buf.toString())

    if (expected.length === 3) {
      t.alike(expected.sort(), [
        'echo: a',
        'echo: bc',
        'echo: d'
      ])

      // TODO: .end() here triggers a bug, investigate
      b.destroy()
    }
  })

  a.trySend(b4a.from('a'))
  a.trySend(b4a.from('bc'))
  a.trySend(b4a.from('d'))
})

test('ipv6 streams', async function (t) {
  t.plan(1)

  const u = new UDX()

  const aSocket = u.createSocket()
  aSocket.bind(0, '::1')
  t.teardown(() => aSocket.close())

  const bSocket = u.createSocket()
  bSocket.bind(0, '::1')
  t.teardown(() => bSocket.close())

  const a = u.createStream(1)
  const b = u.createStream(2)

  a.connect(aSocket, 2, bSocket.address().port, '::1')
  b.connect(bSocket, 1, aSocket.address().port, '::1')

  a.on('data', function (data) {
    t.alike(data, b4a.from('hello world'))
    a.end()
  })

  b.end('hello world')
})

test('several streams on same socket', async function (t) {
  const u = new UDX()

  const socket = u.createSocket()
  socket.bind(0, '127.0.0.1')

  for (let i = 0; i < 10; i++) {
    const stream = u.createStream(i)
    stream.connect(socket, i, socket.address().port)

    t.teardown(() => stream.destroy())
  }

  t.teardown(() => socket.close())
  t.pass('halts')
})

test('destroy unconnected stream', async function (t) {
  t.plan(1)

  const u = new UDX()

  const stream = u.createStream(1)

  stream.on('close', function () {
    t.pass('closed')
  })

  stream.destroy()
})

test('preconnect flow', async function (t) {
  t.plan(9)

  const u = new UDX()

  const socket = u.createSocket()
  socket.bind(0, '127.0.0.1')

  let once = true

  const a = u.createStream(1, {
    firewall (sock, port, host, family) {
      t.ok(once)
      t.is(sock, socket)
      t.is(port, socket.address().port)
      t.is(host, '127.0.0.1')
      t.is(family, 4)
      once = false

      return false
    }
  })

  a.on('data', function (data) {
    t.is(data.toString(), 'hello', 'can receive data preconnect')

    a.connect(socket, 2, socket.address().port)
    a.end()
  })

  const b = u.createStream(2)

  b.connect(socket, 1, socket.address().port)
  b.write(b4a.from('hello'))
  b.end()

  let closed = 0

  b.resume()
  b.on('close', function () {
    t.pass('b closed')
    if (++closed === 2) socket.close()
  })

  a.on('close', function () {
    t.pass('a closed')
    if (++closed === 2) socket.close()
  })

  socket.on('close', function () {
    t.pass('socket closed')
  })
})

test('destroy streams and close socket in callback', async function (t) {
  t.plan(1)

  const u = new UDX()

  const socket = u.createSocket()
  socket.bind(0, '127.0.0.1')

  const a = u.createStream(1)
  const b = u.createStream(2)

  a.connect(socket, 2, socket.address().port)
  b.connect(socket, 1, socket.address().port)

  a.on('data', async function (data) {
    a.destroy()
    b.destroy()

    await socket.close()

    t.pass('closed')
  })

  b.write(b4a.from('hello'))
})

test('write empty buffer', async function (t) {
  t.plan(2)

  const [a, b] = makeTwoStreams(t)

  a
    .on('close', function () {
      t.pass('a closed')
    })
    .end()

  b
    .on('close', function () {
      t.pass('b closed')
    })
    .end(b4a.alloc(0))
})

test('out of order packets', async function (t) {
  t.plan(3)

  const u = new UDX()

  const a = u.createSocket()
  const b = u.createSocket()

  a.bind(0, '127.0.0.1')
  b.bind(0, '127.0.0.1')

  const count = 1000
  const expected = Array(count).fill(0).map((_, i) => i.toString()).join('')
  let received = ''

  const p = await proxy({ from: a, to: b }, async function (pkt) {
    // Add a random delay to every packet
    await new Promise((resolve) =>
      setTimeout(resolve, Math.random() * 1000 | 0)
    )

    return false
  })

  const aStream = u.createStream(1)
  const bStream = u.createStream(2)

  aStream.connect(a, 2, p.address().port)
  bStream.connect(b, 1, p.address().port)

  for (let i = 0; i < count; i++) {
    aStream.write(b4a.from(i.toString()))
  }

  bStream.on('data', function (s) {
    received = received + s.toString()

    if (received.length === expected.length) {
      t.alike(received, expected, 'received in order')

      p.close()
      aStream.destroy()
      bStream.destroy()
    }
  })

  aStream.on('close', function () {
    t.pass('a stream closed')
    b.close()
  })

  bStream.on('close', function () {
    t.pass('b stream closed')
    a.close()
  })
})

test('out of order reads but can destroy (memleak test)', async function (t) {
  t.plan(3)

  const u = new UDX()

  const a = u.createSocket()
  const b = u.createSocket()

  a.bind(0, '127.0.0.1')
  b.bind(0, '127.0.0.1')

  let processed = 0

  const p = await proxy({ from: a, to: b }, function (pkt) {
    if (pkt.data.toString().startsWith('a') && processed > 0) {
      // destroy with out or order packets delivered
      t.pass('close while streams have out of order state')
      p.close()
      aStream.destroy()
      bStream.destroy()
      return true
    }

    return processed++ === 0 // drop first packet
  })

  const aStream = u.createStream(1)
  const bStream = u.createStream(2)

  aStream.connect(a, 2, p.address().port)
  bStream.connect(b, 1, p.address().port)

  aStream.write(b4a.from(Array(1200).fill('a').join('')))
  aStream.write(b4a.from(Array(1000).fill('b').join('')))

  aStream.on('close', function () {
    t.pass('a stream closed')
    b.close()
  })

  bStream.on('close', function () {
    t.pass('b stream closed')
    a.close()
  })
})

test('close socket on stream close', async function (t) {
  t.plan(2)

  const u = new UDX()

  const aSocket = u.createSocket()
  aSocket.bind(0, '127.0.0.1')

  const bSocket = u.createSocket()
  bSocket.bind(0, '127.0.0.1')

  const a = u.createStream(1)
  const b = u.createStream(2)

  a.connect(aSocket, 2, bSocket.address().port)
  b.connect(bSocket, 1, aSocket.address().port)

  a
    .on('close', async function () {
      await aSocket.close()
      t.pass('a closed')
    })
    .end()

  b
    .on('end', function () {
      b.end()
    })
    .on('close', async function () {
      await bSocket.close()
      t.pass('b closed')
    })
})

test('write string', async function (t) {
  t.plan(3)

  const [a, b] = makeTwoStreams(t)

  a
    .on('data', function (data) {
      t.alike(data, b4a.from('hello world'))
    })
    .on('close', function () {
      t.pass('a closed')
    })
    .end()

  b
    .on('close', function () {
      t.pass('b closed')
    })
    .end('hello world')
})

test('destroy before fully connected', async function (t) {
  t.plan(2)

  const u = new UDX()

  const socket = u.createSocket()
  socket.bind(0, '127.0.0.1')

  const a = u.createStream(1)
  const b = u.createStream(2, {
    firewall () {
      return false // accept packets from a
    }
  })

  a.connect(socket, 2, socket.address().port)
  a.destroy()

  b
    .on('error', function (err) {
      t.is(err.code, 'ECONNRESET')
    })
    .on('close', async function () {
      t.pass('b closed')
      await socket.close()
    })

  setTimeout(function () {
    b.connect(socket, 1, socket.address().port)
    b.destroy()
  }, 100) // wait for destroy to be processed
})

test('throw in data callback', async function (t) {
  t.plan(1)

  const u = new UDX()

  const socket = u.createSocket()
  socket.bind(0, '127.0.0.1')

  const a = u.createStream(1)
  const b = u.createStream(2)

  a.connect(socket, 2, socket.address().port)
  b.connect(socket, 1, socket.address().port)

  a.on('data', function () {
    throw new Error('boom')
  })

  b.end(b4a.from('hello'))

  uncaught(async (err) => {
    t.is(err.message, 'boom')

    a.destroy()
    b.destroy()

    await socket.close()
  })
})

test('throw in message callback', async function (t) {
  t.plan(1)

  const u = new UDX()

  const socket = u.createSocket()
  socket.bind(0, '127.0.0.1')

  const a = u.createStream(1)
  const b = u.createStream(2)

  a.connect(socket, 2, socket.address().port)
  b.connect(socket, 1, socket.address().port)

  a.on('message', function () {
    throw new Error('boom')
  })

  b.send(b4a.from('hello'))

  uncaught(async (err) => {
    t.is(err.message, 'boom')

    a.destroy()
    b.destroy()

    await socket.close()
  })
})

test('seq and ack wraparound', async function (t) {
  t.plan(1)

  const u = new UDX()

  let received = ''
  // enough data to fill 10 packets
  const expected = Array(1500 * 10).join('a')

  const socket = u.createSocket()
  socket.bind(0, '127.0.0.1')

  const a = u.createStream(1, { seq: 2 ** 32 - 5 })
  t.teardown(() => a.destroy())

  const b = u.createStream(2)
  t.teardown(() => b.destroy())

  t.teardown(() => socket.close())

  a.connect(socket, 2, socket.address().port)
  b.connect(socket, 1, socket.address().port, { ack: 2 ** 32 - 5 })

  b.on('data', function (i) {
    received = received + i.toString()
    if (expected.length === received.length) {
      t.alike(expected, received)
    }
  })

  a.write(expected)
})

test('busy and idle events', async function (t) {
  t.plan(10)

  const udx = new UDX()

  const socket = udx.createSocket()
  socket.bind(0, '127.0.0.1')

  let idle = false
  let busy = false

  socket
    .on('idle', function () {
      idle = true
      busy = false

      socket.close()
    })
    .on('busy', function () {
      busy = true
      idle = false
    })

  const stream = udx.createStream(1)

  t.absent(idle)
  t.absent(busy)

  stream
    .on('connect', function () {
      t.absent(idle)
      t.ok(busy)

      t.is(idle, socket.idle)
      t.is(busy, socket.busy)

      stream.destroy()
    })
    .on('close', function () {
      t.ok(idle)
      t.absent(busy)

      t.is(idle, socket.idle)
      t.is(busy, socket.busy)
    })
    .connect(socket, 2, socket.address().port)
})

test('no idle after close', async function (t) {
  t.plan(1)

  const udx = new UDX()

  const socket = udx.createSocket()
  socket.bind(0, '127.0.0.1')

  const stream = udx.createStream(1)

  stream
    .on('connect', function () {
      stream.destroy()

      socket
        .on('idle', function () {
          t.fail('idle event after close')
        })
        .on('close', function () {
          t.pass('socket closed')
        })
        .close()
    })
    .connect(socket, 2, socket.address().port)
})

test('localHost, localFamily and localPort', async function (t) {
  t.plan(6)

  const udx = new UDX()

  const socket = udx.createSocket()
  socket.bind(0, '127.0.0.1')

  const stream = udx.createStream(1)

  t.is(stream.localHost, null)
  t.is(stream.localFamily, 0)
  t.is(stream.localPort, 0)

  stream.on('connect', function () {
    t.is(stream.localHost, '127.0.0.1')
    t.is(stream.localFamily, 4)
    t.is(typeof stream.localPort, 'number')

    stream.destroy()
    socket.close()
  })

  stream.connect(socket, 2, socket.address().port, '127.0.0.1')
})

test('write to unconnected stream', async function (t) {
  t.plan(1)

  const udx = new UDX()

  const socket = udx.createSocket()
  socket.bind(0, '127.0.0.1')

  const stream = udx.createStream(1)
  stream.write(b4a.alloc(0))

  uncaught(function (error) {
    t.is(error.code, 'ERR_ASSERTION')

    stream.destroy()
    socket.close()
  })
})

test('backpressures stream', async function (t) {
  t.plan(2)

  const u = new UDX()

  const send = 512 * 1024 * 1024

  let sent = 0
  let recv = 0

  const socket = u.createSocket()
  socket.bind(0, '127.0.0.1')

  const a = u.createStream(1)
  const b = u.createStream(2)

  a.connect(socket, 2, socket.address().port)
  b.connect(socket, 1, socket.address().port)

  const rs = new Readable({
    read (cb) {
      sent += 65536
      this.push(Buffer.alloc(65536))
      if (sent === send) this.push(null)
      cb(null)
    }
  })

  rs.pipe(a)

  b.resume()
  b.on('data', function (data) {
    recv += data.byteLength
  })
  b.on('end', function () {
    t.is(recv, send)
    t.ok(send > 0, 'sanity check, sent ' + send + ' bytes')

    b.end()
    socket.close()
  })
})

test('UDX - basic stats', async function (t) {
  const tWave1 = t.test()
  tWave1.plan(1)

  const tWave2 = t.test()
  tWave2.plan(1)

  const [a, b] = makeTwoStreams(t)
  const aUdx = a.udx

  t.is(a.bytesTransmitted, 0, 'sanity check: init 0')
  t.is(a.packetsTransmitted, 0, 'sanity check: init 0')
  t.is(a.bytesReceived, 0, 'sanity check: init 0')
  t.is(a.packetsReceived, 0, 'sanity check: init 0')
  t.is(b.bytesTransmitted, 0, 'sanity check: init 0')
  t.is(b.packetsTransmitted, 0, 'sanity check: init 0')
  t.is(b.bytesReceived, 0, 'sanity check: init 0')
  t.is(b.packetsReceived, 0, 'sanity check: init 0')

  t.is(aUdx.bytesTransmitted, 0, 'sanity check: init 0')
  t.is(aUdx.packetsTransmitted, 0, 'sanity check: init 0')
  t.is(aUdx.bytesReceived, 0, 'sanity check: init 0')
  t.is(aUdx.packetsReceived, 0, 'sanity check: init 0')

  t.is(a.retransmits, 0, 'initialized to zero')
  t.is(a.rtoCount, 0, 'initialized to zero')
  t.is(a.fastRecoveries, 0, 'initialized to zero')

  let aNrDataEvents = 0
  a.on('data', function (data) {
    if (++aNrDataEvents === 1) {
      tWave1.pass('a received the first echo packet')
    }

    if (aNrDataEvents < 10) {
      b.write(b4a.from('creating imbalance'.repeat(100)))
    }

    if (aNrDataEvents === 10) {
      tWave2.pass('imbalance created')
    }
  })

  b.on('data', function (data) {
    b.write(b4a.concat([b4a.from('echo: '), data]))
  })

  a.write(b4a.from('hello world'))

  await tWave1

  // Pretty hard to calculate the exact amounts of expected packets/bytes
  // so we just sanity check the ballpark
  t.is(a.bytesTransmitted > 20, true, `a reasonable bytesTransmitted (${a.bytesTransmitted})`)
  t.is(a.packetsTransmitted > 0, true, `a reasonable packetsTransmitted (${a.packetsTransmitted})`)
  t.is(a.bytesReceived > 2, true, `a reasonable bytesReceived (${a.bytesReceived})`)
  t.is(a.packetsReceived > 0, true, `a reasonable packetsReceived (${a.packetsReceived})`)
  t.is(b.bytesTransmitted > 20, true, `b reasonable bytesTransmitted (${b.bytesTransmitted})`)
  t.is(b.packetsTransmitted > 0, true, `b reasonable packetsTransmitted (${b.packetsTransmitted})`)
  t.is(b.bytesReceived > 20, true, `b reasonable bytesReceived (${b.bytesReceived})`)
  t.is(b.packetsReceived > 0, true, `b reasonable packetsReceived (${b.packetsReceived})`)

  await tWave2
  a.end()
  b.end()

  t.is(a.bytesReceived > 1000, true, `a now higher bytesReceived (${a.bytesReceived})`)
  t.is(b.bytesTransmitted > 1000, true, `b now higher bytesTransmitted (${b.bytesTransmitted})`)

  t.is(aUdx.bytesTransmitted, a.bytesTransmitted, `udx same bytes out as the single stream (${aUdx.bytesTransmitted})`)
  t.is(aUdx.packetsTransmitted, a.packetsTransmitted, `udx same packets out as the single stream (${aUdx.packetsTransmitted})`)
  t.is(aUdx.bytesReceived, a.bytesReceived, `udx same bytes in as the single stream (${aUdx.bytesReceived})`)
  t.is(aUdx.packetsReceived, a.packetsReceived, true, `udx same packets in as the single stream (${aUdx.packetsReceived})`)

  const aSocket = a.socket
  t.is(aSocket.bytesTransmitted, a.bytesTransmitted, `udx socket same bytes out as the single stream (${aSocket.bytesTransmitted})`)
  t.is(aSocket.packetsTransmitted, a.packetsTransmitted, `udx socket same packets out as the single stream (${aSocket.packetsTransmitted})`)
  t.is(aSocket.bytesReceived, a.bytesReceived, `udx socket same bytes in as the single stream (${aSocket.bytesReceived})`)
  t.is(aSocket.packetsReceived, a.packetsReceived, true, `udx socket same packets in as the single stream (${aSocket.packetsReceived})`)
})
