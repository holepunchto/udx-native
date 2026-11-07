const test = require('brittle')
const b4a = require('b4a')
const { makeTwoStreams } = require('./helpers')

test('framed mode', function (t) {
  t.plan(1)

  const [a, b] = makeTwoStreams(t, { framed: true })

  b.on('data', (buffer) => {
    t.alike(buffer, b4a.from([0x2, 0x0, 0x0, 0x4, 0x5]))

    a.destroy()
    b.destroy()
  })

  a.write(b4a.from([0x2, 0x0, 0x0]))
  a.write(b4a.from([0x4, 0x5]))
})

test('framed mode, large message', function (t) {
  t.plan(1)

  const [a, b] = makeTwoStreams(t, { framed: true })

  const buf = b4a.alloc(3 + 1024 * 4096 /* 4 MiB */)

  buf[2] = 0x40

  const recv = []

  b.on('data', (buffer) => {
    recv.push(buffer)
  }).on('end', () => {
    t.alike(b4a.concat(recv), buf)

    a.destroy()
    b.destroy()
  })

  a.end(buf)
})

test('framed mode, several frames', function (t) {
  t.plan(1)

  const [a, b] = makeTwoStreams(t, { framed: true })

  const buf = b4a.from([0x2, 0x0, 0x0, 0x4, 0x5])

  const recv = []

  b.on('data', (buffer) => {
    recv.push(buffer)
  }).on('end', () => {
    t.alike(b4a.concat(recv), b4a.concat([buf, buf, buf]))

    a.destroy()
    b.destroy()
  })

  a.write(buf)
  a.write(buf)
  a.end(buf)
})

for (const split of [1, 2]) {
  test(`framed mode, header split ${split} + ${3 - split} across packets`, function (t) {
    t.plan(1)

    const [a, b] = makeTwoStreams(t, { framed: true })

    const frame = b4a.from([0x2, 0x0, 0x0, 0x4, 0x5])

    b.on('data', (buffer) => {
      t.alike(buffer, frame)

      a.destroy()
      b.destroy()
    })

    a.write(frame.subarray(0, split))
    setTimeout(() => a.write(frame.subarray(split)), 50)
  })
}

test('framed mode, stays batched after a packet spans two frames', function (t) {
  t.plan(2)

  const [a, b] = makeTwoStreams(t, { framed: true })

  // frame ends fall mid-packet, so packets carry the end of one frame and the start of the next
  const frames = []
  for (let i = 0; i < 64; i++) {
    const frame = b4a.alloc(3 + 65000, i)
    frame.writeUIntLE(65000, 0, 3)
    frames.push(frame)
  }
  const sent = b4a.concat(frames)

  const recv = []

  b.on('data', (buffer) => {
    recv.push(buffer)
  }).on('end', () => {
    t.alike(b4a.concat(recv), sent)
    t.ok(sent.byteLength / recv.length > 8192, `${recv.length} reads for ${sent.byteLength} bytes`)

    a.destroy()
    b.destroy()
  })

  for (const frame of frames) a.write(frame)
  a.end()
})

test('framed mode, invalid frame', function (t) {
  t.plan(1)

  const [a, b] = makeTwoStreams(t, { framed: true })

  const buf = b4a.from([0x1 /* too short, leftover data */, 0x0, 0x0, 0x4, 0x5])

  b.on('data', (buffer) => {
    t.alike(buffer, buf)

    a.destroy()
    b.destroy()
  })

  a.write(buf)
})

test('framed mode, frames back to back without end', function (t) {
  t.plan(1)

  const [a, b] = makeTwoStreams(t, { framed: true })
  const frames = []
  for (const len of [140000, 0, 2000]) {
    const frame = b4a.alloc(3 + len, 0xff)
    frame.writeUIntLE(len, 0, 3)
    frames.push(frame)
  }
  const sent = b4a.concat(frames)

  const recv = []
  let received = 0

  b.on('data', (buffer) => {
    recv.push(buffer)
    received += buffer.byteLength

    if (received === sent.byteLength) {
      t.alike(b4a.concat(recv), sent)

      a.destroy()
      b.destroy()
    }
  })

  a.write(sent)
})
