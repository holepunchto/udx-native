const UDX = require('../..')

// from udx.h

const UDX_HEADER_SIZE = 20
const UDX_MAGIC_BYTE = 0xff
const UDX_VERSION = 1
const UDX_HEADER_DATA = 0b00001
const UDX_HEADER_END = 0b00010
const UDX_HEADER_SACK = 0b00100
const UDX_HEADER_MESSAGE = 0b01000
const UDX_HEADER_DESTROY = 0b10000

module.exports = function proxy({ from, to, bind } = {}, drop) {
  from = toPort(from)
  to = toPort(to)

  const u = new UDX()
  const socket = u.createSocket()

  socket.bind(bind || 0)

  socket.on('message', function (buf, rinfo) {
    const source = {
      host: rinfo.address,
      port: rinfo.port,
      peer: rinfo.port === from ? 'from' : rinfo.port === to ? 'to' : 'unknown'
    }
    const pkt = parsePacket(buf, source)
    const dropping = drop(pkt, source)
    const port = rinfo.port === to ? from : to

    if (dropping && dropping.then) dropping.then(fwd).catch(noop)
    else fwd(dropping)

    function fwd(dropping) {
      if (dropping === true) return
      socket.send(buf, port, '127.0.0.1')
    }
  })

  return socket
}

function toPort(n) {
  if (typeof n === 'number') return n
  if (n && n.address) return n.address().port
  throw new Error('Pass a port or socket')
}

function echo(s) {
  return s
}

function prettyPrint(pkt, { peer }, opts) {
  const style = (opts && opts.stylize) || echo

  let s = ''
  if (peer === 'from') s += 'from ' + style('-->', 'symbol') + ' to'
  else if (peer === 'to') s += 'from ' + style('<--', 'date') + ' to'
  else s += 'unknown'

  s += ': '

  if (pkt.protocol !== 'udx') {
    s += style('unknown', 'symbol') + ' data=' + opts.stylize(pkt.data)
    return s
  }

  const flags = []

  if (pkt.isData) flags.push(style('data', 'special'))
  if (pkt.isEnd) flags.push(style('end', 'special'))
  if (pkt.isSack) flags.push(style('sack', 'special'))
  if (pkt.isMessage) flags.push(style('message', 'special'))
  if (pkt.isDestroy) flags.push(style('destroy', 'special'))

  if (!flags.length) flags.push(style('state', 'special'))

  s += flags.join('+') + ' '
  s += 'stream=' + opts.stylize(pkt.stream, opts) + ' '
  s += 'recv=' + opts.stylize(pkt.recv, opts) + ' '
  s += 'seq=' + opts.stylize(pkt.seq, opts) + ' '
  s += 'ack=' + opts.stylize(pkt.ack, opts) + ' '

  if (pkt.additionalHeader.byteLength) {
    s += 'additional = ' + opts.stylize(pkt.additionalHeader, opts) + ' '
  }
  if (pkt.data.byteLength) s += 'data=' + opts.stylize(pkt.data, opts)

  s = s.trim()

  return s
}

function parsePacket(buf, source) {
  if (buf.byteLength < UDX_HEADER_SIZE || buf[0] !== UDX_MAGIC_BYTE || buf[1] !== UDX_VERSION) {
    return { protocol: 'unknown', buffer: buf }
  }

  const type = buf[2]
  const dataOffset = buf[3]

  const inspect = Symbol.for('nodejs.util.inspect.custom')

  return {
    protocol: 'udx',
    version: buf[1],
    isData: !!(type & UDX_HEADER_DATA),
    isEnd: !!(type & UDX_HEADER_END),
    isSack: !!(type & UDX_HEADER_SACK),
    isMessage: !!(type & UDX_HEADER_MESSAGE),
    isDestroy: !!(type & UDX_HEADER_DESTROY),
    stream: buf.readUInt32LE(4),
    recv: buf.readUInt32LE(8),
    seq: buf.readUInt32LE(12),
    ack: buf.readUInt32LE(16),
    additionalHeader: buf.subarray(UDX_HEADER_SIZE, UDX_HEADER_SIZE + dataOffset),
    data: buf.subarray(UDX_HEADER_SIZE + dataOffset),
    [inspect](depth, opts) {
      return prettyPrint(this, source, opts)
    }
  }
}

function noop() {}
