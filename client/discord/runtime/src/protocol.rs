//! Wire-format codec, byte-compatible with `app/discord_protocol.py`.
//!
//! The server (`app/discord_http3.py`) and the Python client both use this
//! envelope, so the embedded runtime must speak exactly the same frames.

pub const MAX_WIRE_DATAGRAM: usize = 1150;
pub const MAX_DATAGRAM_PAYLOAD: usize = 65_507;
pub const MAX_PIPE_PAYLOAD: usize = 1024 * 1024;
pub const MAX_FRAGMENT_COUNT: usize = 64;
const MAX_FRAGMENT_ASSEMBLIES: usize = 128;
const FRAGMENT_MAGIC: &[u8; 4] = b"DRF1";
const FRAGMENT_HEADER_SIZE: usize = 12;

pub type FragmentKey = (u32, String, u16, u32);
pub type FragmentState = std::collections::HashMap<FragmentKey, Vec<Option<Vec<u8>>>>;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ProtocolError {
    MissingAddressType,
    TruncatedIpv4,
    TruncatedIpv6,
    MissingDomainLength,
    TruncatedDomain,
    EmptyDomain,
    UnsupportedAddressType,
    DatagramTooShort,
    MissingPort,
    PortOutOfRange,
    PayloadTooLarge,
    TrailingBytes,
    InvalidFragment,
}

impl std::fmt::Display for ProtocolError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{self:?}")
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Address {
    Ipv4([u8; 4]),
    Ipv6([u8; 16]),
    Domain(String),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DatagramPacket {
    pub flow_id: u32,
    pub host: Address,
    pub port: u16,
    pub payload: Vec<u8>,
}

const ADDR_IPV4: u8 = 1;
const ADDR_IPV6: u8 = 4;
const ADDR_DOMAIN: u8 = 3;

pub fn encode_address(host: &Address) -> Vec<u8> {
    let mut out = Vec::with_capacity(24);
    match host {
        Address::Ipv4(octets) => {
            out.push(ADDR_IPV4);
            out.extend(octets);
        }
        Address::Ipv6(octets) => {
            out.push(ADDR_IPV6);
            out.extend(octets);
        }
        Address::Domain(name) => {
            out.push(ADDR_DOMAIN);
            let bytes = name.as_bytes();
            debug_assert!(bytes.len() <= 255);
            out.push(bytes.len() as u8);
            out.extend(bytes);
        }
    }
    out
}

pub fn decode_address(data: &[u8], mut offset: usize) -> Result<(Address, usize), ProtocolError> {
    let kind = *data.get(offset).ok_or(ProtocolError::MissingAddressType)?;
    offset += 1;
    match kind {
        ADDR_IPV4 => {
            let end = offset + 4;
            if end > data.len() {
                return Err(ProtocolError::TruncatedIpv4);
            }
            Ok((Address::Ipv4(data[offset..end].try_into().unwrap()), end))
        }
        ADDR_IPV6 => {
            let end = offset + 16;
            if end > data.len() {
                return Err(ProtocolError::TruncatedIpv6);
            }
            Ok((Address::Ipv6(data[offset..end].try_into().unwrap()), end))
        }
        ADDR_DOMAIN => {
            let len = *data.get(offset).ok_or(ProtocolError::MissingDomainLength)? as usize;
            offset += 1;
            let end = offset + len;
            if len == 0 {
                return Err(ProtocolError::EmptyDomain);
            }
            if end > data.len() {
                return Err(ProtocolError::TruncatedDomain);
            }
            match std::str::from_utf8(&data[offset..end]) {
                Ok(s) => Ok((Address::Domain(s.to_owned()), end)),
                Err(_) => Err(ProtocolError::TruncatedDomain),
            }
        }
        _ => Err(ProtocolError::UnsupportedAddressType),
    }
}

pub fn encode_datagram(packet: &DatagramPacket) -> Result<Vec<u8>, ProtocolError> {
    if packet.payload.len() > MAX_DATAGRAM_PAYLOAD {
        return Err(ProtocolError::PayloadTooLarge);
    }
    if packet.port == 0 {
        return Err(ProtocolError::PortOutOfRange);
    }
    let host = match &packet.host {
        Address::Ipv4(value) => std::net::Ipv4Addr::from(*value).to_string(),
        Address::Ipv6(value) => std::net::Ipv6Addr::from(*value).to_string(),
        Address::Domain(value) => value.clone(),
    };
    let host = host.as_bytes();
    if host.is_empty() || host.len() > u8::MAX as usize {
        return Err(ProtocolError::EmptyDomain);
    }
    let mut out = Vec::with_capacity(packet.payload.len() + 7 + host.len());
    out.extend_from_slice(&packet.flow_id.to_be_bytes());
    out.push(host.len() as u8);
    out.extend_from_slice(&packet.port.to_be_bytes());
    out.extend_from_slice(host);
    out.extend_from_slice(&packet.payload);
    if out.len() > MAX_WIRE_DATAGRAM {
        return Err(ProtocolError::PayloadTooLarge);
    }
    Ok(out)
}

pub fn decode_datagram(data: &[u8]) -> Result<DatagramPacket, ProtocolError> {
    if data.len() > MAX_WIRE_DATAGRAM {
        return Err(ProtocolError::PayloadTooLarge);
    }
    if data.len() < 8 {
        return Err(ProtocolError::DatagramTooShort);
    }
    let flow_id = u32::from_be_bytes(data[..4].try_into().unwrap());
    let host_len = data[4] as usize;
    let end = 7 + host_len;
    if host_len == 0 {
        return Err(ProtocolError::EmptyDomain);
    }
    let host_bytes = data.get(7..end).ok_or(ProtocolError::TruncatedDomain)?;
    let host_text = std::str::from_utf8(host_bytes).map_err(|_| ProtocolError::TruncatedDomain)?;
    let host = match host_text.parse::<std::net::IpAddr>() {
        Ok(std::net::IpAddr::V4(value)) => Address::Ipv4(value.octets()),
        Ok(std::net::IpAddr::V6(value)) => Address::Ipv6(value.octets()),
        Err(_) => Address::Domain(host_text.to_owned()),
    };
    let port_bytes = &data[5..7];
    let port = u16::from_be_bytes(port_bytes.try_into().unwrap());
    if port == 0 {
        return Err(ProtocolError::PortOutOfRange);
    }
    let payload = data[end..].to_vec();
    if payload.len() > MAX_DATAGRAM_PAYLOAD {
        return Err(ProtocolError::PayloadTooLarge);
    }
    Ok(DatagramPacket {
        flow_id,
        host,
        port,
        payload,
    })
}

pub fn fragment_datagram(
    packet: &DatagramPacket,
    message_id: u32,
) -> Result<Vec<DatagramPacket>, ProtocolError> {
    if packet.payload.len() > MAX_DATAGRAM_PAYLOAD {
        return Err(ProtocolError::PayloadTooLarge);
    }
    let host_len = address_string(&packet.host).len();
    if 7 + host_len + packet.payload.len() <= MAX_WIRE_DATAGRAM {
        return Ok(vec![packet.clone()]);
    }
    let chunk_size = MAX_WIRE_DATAGRAM
        .checked_sub(7 + host_len + FRAGMENT_HEADER_SIZE)
        .ok_or(ProtocolError::PayloadTooLarge)?;
    let count = packet.payload.len().div_ceil(chunk_size);
    if !(2..=MAX_FRAGMENT_COUNT).contains(&count) {
        return Err(ProtocolError::PayloadTooLarge);
    }
    Ok(packet
        .payload
        .chunks(chunk_size)
        .enumerate()
        .map(|(index, chunk)| {
            let mut payload = Vec::with_capacity(FRAGMENT_HEADER_SIZE + chunk.len());
            payload.extend_from_slice(FRAGMENT_MAGIC);
            payload.extend_from_slice(&message_id.to_be_bytes());
            payload.extend_from_slice(&(index as u16).to_be_bytes());
            payload.extend_from_slice(&(count as u16).to_be_bytes());
            payload.extend_from_slice(chunk);
            DatagramPacket {
                flow_id: packet.flow_id,
                host: packet.host.clone(),
                port: packet.port,
                payload,
            }
        })
        .collect())
}

pub fn reassemble_datagram(
    packet: DatagramPacket,
    state: &mut FragmentState,
) -> Result<Option<DatagramPacket>, ProtocolError> {
    if !packet.payload.starts_with(FRAGMENT_MAGIC) {
        return Ok(Some(packet));
    }
    if packet.payload.len() < FRAGMENT_HEADER_SIZE {
        return Err(ProtocolError::InvalidFragment);
    }
    let message_id = u32::from_be_bytes(packet.payload[4..8].try_into().unwrap());
    let index = u16::from_be_bytes(packet.payload[8..10].try_into().unwrap()) as usize;
    let count = u16::from_be_bytes(packet.payload[10..12].try_into().unwrap()) as usize;
    if !(2..=MAX_FRAGMENT_COUNT).contains(&count) || index >= count {
        return Err(ProtocolError::InvalidFragment);
    }
    let key = (
        packet.flow_id,
        address_string(&packet.host),
        packet.port,
        message_id,
    );
    if !state.contains_key(&key) && state.len() >= MAX_FRAGMENT_ASSEMBLIES {
        if let Some(oldest) = state.keys().next().cloned() {
            state.remove(&oldest);
        }
    }
    let parts = state
        .entry(key.clone())
        .or_insert_with(|| vec![None; count]);
    if parts.len() != count {
        state.remove(&key);
        return Err(ProtocolError::InvalidFragment);
    }
    parts[index] = Some(packet.payload[FRAGMENT_HEADER_SIZE..].to_vec());
    if parts.iter().any(Option::is_none) {
        return Ok(None);
    }
    let payload: Vec<u8> = parts.iter().flatten().flatten().copied().collect();
    state.remove(&key);
    if payload.len() > MAX_DATAGRAM_PAYLOAD {
        return Err(ProtocolError::PayloadTooLarge);
    }
    Ok(Some(DatagramPacket { payload, ..packet }))
}

fn address_string(host: &Address) -> String {
    match host {
        Address::Ipv4(value) => std::net::Ipv4Addr::from(*value).to_string(),
        Address::Ipv6(value) => std::net::Ipv6Addr::from(*value).to_string(),
        Address::Domain(value) => value.clone(),
    }
}

pub fn encode_target(host: &Address, port: u16) -> Result<Vec<u8>, ProtocolError> {
    if port == 0 {
        return Err(ProtocolError::PortOutOfRange);
    }
    let mut out = encode_address(host);
    out.extend_from_slice(&port.to_be_bytes());
    Ok(out)
}

pub fn decode_target(data: &[u8]) -> Result<(Address, u16), ProtocolError> {
    let (host, offset) = decode_address(data, 0)?;
    let port_bytes = data
        .get(offset..offset + 2)
        .ok_or(ProtocolError::MissingPort)?;
    let port = u16::from_be_bytes(port_bytes.try_into().unwrap());
    if port == 0 || offset + 2 != data.len() {
        return Err(ProtocolError::PortOutOfRange);
    }
    Ok((host, port))
}

pub fn is_global_ipv4(octets: [u8; 4]) -> bool {
    // Match Python `ipaddress.ip_address(...).is_global` for the common cases
    // the server cares about: reject private/loopback/link-local ranges.
    let [a, b, _, _] = octets;
    match a {
        0 => false,                          // 0.0.0.0/8
        10 => false,                         // 10.0.0.0/8
        100 if b >= 64 && b <= 127 => false, // 100.64.0.0/10
        127 => false,                        // loopback
        169 if b == 254 => false,            // link-local
        172 if (16..=31).contains(&b) => false,
        192 if b == 168 => false,           // private
        198 if b == 18 || b == 19 => false, // benchmarking
        _ => true,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn datagram_roundtrip_ipv4() {
        let packet = DatagramPacket {
            flow_id: 0xDEAD_BEEF,
            host: Address::Ipv4([93, 184, 216, 34]),
            port: 53,
            payload: vec![1, 2, 3, 4, 5],
        };
        let encoded = encode_datagram(&packet).unwrap();
        let mut server_wire = vec![0xde, 0xad, 0xbe, 0xef, 13, 0, 53];
        server_wire.extend_from_slice(b"93.184.216.34");
        server_wire.extend_from_slice(&[1, 2, 3, 4, 5]);
        assert_eq!(encoded, server_wire);
        let decoded = decode_datagram(&encoded).unwrap();
        assert_eq!(decoded, packet);
    }

    #[test]
    fn datagram_roundtrip_domain_and_v6() {
        for host in [
            Address::Domain("discord.media".into()),
            Address::Ipv6([0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1]),
        ] {
            let packet = DatagramPacket {
                flow_id: 1,
                host,
                port: 50000,
                payload: b"x".to_vec(),
            };
            let decoded = decode_datagram(&encode_datagram(&packet).unwrap()).unwrap();
            assert_eq!(decoded, packet);
        }
    }

    #[test]
    fn rejects_oversized_payload() {
        let packet = DatagramPacket {
            flow_id: 1,
            host: Address::Ipv4([1, 1, 1, 1]),
            port: 80,
            payload: vec![0u8; MAX_DATAGRAM_PAYLOAD + 1],
        };
        assert!(matches!(
            encode_datagram(&packet),
            Err(ProtocolError::PayloadTooLarge)
        ));
    }

    #[test]
    fn large_datagram_fragmentation_round_trip() {
        let packet = DatagramPacket {
            flow_id: 42,
            host: Address::Ipv4([104, 29, 145, 255]),
            port: 19340,
            payload: (0..1536).map(|value| value as u8).collect(),
        };
        let fragments = fragment_datagram(&packet, 7).unwrap();
        assert!(fragments.len() > 1);
        assert!(fragments
            .iter()
            .all(|fragment| encode_datagram(fragment).unwrap().len() <= MAX_WIRE_DATAGRAM));
        let mut state = FragmentState::new();
        let mut rebuilt = None;
        for fragment in fragments.into_iter().rev() {
            rebuilt = reassemble_datagram(fragment, &mut state).unwrap().or(rebuilt);
        }
        assert_eq!(rebuilt, Some(packet));
    }
}
