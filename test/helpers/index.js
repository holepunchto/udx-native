const b4a = require('b4a')
const UDX = require('../../')

module.exports = { makeTwoStreams, makePairs, pipeStreamPairs, uncaught }

function uncaught (fn) {
  if (global.Bare) {
    global.Bare.once('uncaughtException', fn)
  } else {
    process.once('uncaughtException', fn)
  }
}

function makeTwoStreams (t, opts) {
  const a = new UDX()
  const b = new UDX()

  const aSocket = a.createSocket()
  const bSocket = b.createSocket()

  aSocket.bind(0, '127.0.0.1')
  bSocket.bind(0, '127.0.0.1')

  const aStream = a.createStream(1, opts)
  const bStream = b.createStream(2, opts)

  aStream.connect(aSocket, bStream.id, bSocket.address().port, '127.0.0.1')
  bStream.connect(bSocket, aStream.id, aSocket.address().port, '127.0.0.1')

  t.teardown(() => {
    aSocket.close()
    bSocket.close()
  })

  return [aStream, bStream]
}

function makePairs (n, multiplexMode = 'single') {
  const ua = new UDX()
  const ub = new UDX()

  let id = 1
  const sockets = []
  const streams = []
  let a, b
  if (multiplexMode === 'single') {
    a = ua.createSocket()
    b = ub.createSocket()
    a.bind(0, '127.0.0.1')
    b.bind(0, '127.0.0.1')
    sockets.push(a, b)
  }
  while (streams.length < n) {
    let sa, sb
    if (multiplexMode === 'single') {
      sa = a
      sb = b
    } else {
      sa = ua.createSocket()
      sb = ub.createSocket()
      sa.bind(0, '127.0.0.1')
      sb.bind(0, '127.0.0.1')
      sockets.push(sa, sb)
    }
    const streamId = id++
    const aStream = ua.createStream(streamId)
    const bStream = ub.createStream(streamId)
    aStream.connect(sa, bStream.id, sb.address().port, '127.0.0.1')
    bStream.connect(sb, aStream.id, sa.address().port, '127.0.0.1')
    streams.push([aStream, bStream])
  }

  function close () {
    for (const pair of streams) {
      pair[0].destroy()
      pair[1].destroy()
    }
    for (const socket of sockets) {
      socket.close()
    }
  }

  return { sockets, streams, close }
}

async function pipeStreamPairs (streams, messageSize, limit) {
  const msg = b4a.alloc(messageSize, 'a')
  const proms = []
  for (const pair of streams) {
    const [streamA, streamB] = pair
    proms.push(write(streamA, limit, msg))
    proms.push(read(streamB, limit))
  }
  return Promise.all(proms)
  function write (s, limit, msg) {
    return new Promise((resolve, reject) => {
      let written = 0
      s.once('error', reject)
      write()
      function write () {
        let floating = true
        while (floating && written < limit) {
          floating = s.write(msg)
          written += msg.length
        }
        if (written >= limit) {
          resolve()
        } else {
          s.once('drain', write)
        }
      }
    })
  }

  function read (s, limit) {
    return new Promise((resolve, reject) => {
      let read = 0
      s.once('error', reject)
      s.on('data', (data) => {
        read += data.length
        if (read >= limit) {
          resolve()
        }
      })
    })
  }
}
