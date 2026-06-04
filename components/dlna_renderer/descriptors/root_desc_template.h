// Auto-included by dlna_http.c. Single string template, %s substitutions:
//   1. friendlyName  (e.g. "ESP32 AirPlay")
//   2. UDN uuid:...   (e.g. "uuid:9ab38e5a-...-...")
// Minimum UPnP MediaRenderer:1 root descriptor accepted by BubbleUPnP, Plex,
// VLC, Android Cast, Synology DS Audio. dlna:X_DLNADOC=DMR-1.50 is what makes
// Android system-level "Cast" recognise the device as a DMR.
#pragma once

static const char DLNA_ROOT_DESC_TEMPLATE[] =
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
"<root xmlns=\"urn:schemas-upnp-org:device-1-0\""
" xmlns:dlna=\"urn:schemas-dlna-org:device-1-0\">"
"<specVersion><major>1</major><minor>0</minor></specVersion>"
"<device>"
"<dlna:X_DLNADOC>DMR-1.50</dlna:X_DLNADOC>"
"<deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>"
"<friendlyName>%s</friendlyName>"
"<manufacturer>airplay-esp32</manufacturer>"
"<manufacturerURL>https://github.com/</manufacturerURL>"
"<modelDescription>ESP32 UPnP Renderer</modelDescription>"
"<modelName>ESP32-DMR</modelName>"
"<modelNumber>1.0</modelNumber>"
"<serialNumber>0001</serialNumber>"
"<UDN>%s</UDN>"
"<serviceList>"
"<service>"
"<serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType>"
"<serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>"
"<SCPDURL>/dlna/AVTransport.xml</SCPDURL>"
"<controlURL>/dlna/AVTransport/ctrl</controlURL>"
"<eventSubURL>/dlna/AVTransport/evt</eventSubURL>"
"</service>"
"<service>"
"<serviceType>urn:schemas-upnp-org:service:ConnectionManager:1</serviceType>"
"<serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId>"
"<SCPDURL>/dlna/ConnectionManager.xml</SCPDURL>"
"<controlURL>/dlna/ConnectionManager/ctrl</controlURL>"
"<eventSubURL>/dlna/ConnectionManager/evt</eventSubURL>"
"</service>"
"</serviceList>"
"</device>"
"</root>";
