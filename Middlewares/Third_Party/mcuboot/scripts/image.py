# Copyright 2018 Nordic Semiconductor ASA
# Copyright 2017 Linaro Limited
# Copyright 2019-2020 Arm Limited
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""
Image signing and management.
"""

import version as versmod
from boot_record import create_sw_component_data
import click
from enum import Enum
from intelhex import IntelHex
import hashlib
import struct
import os.path
from keys import RSAPublic, ECDSA256P1Public
from cryptography.hazmat.primitives.asymmetric import ec, padding
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat
from cryptography.hazmat.backends import default_backend
from cryptography.hazmat.primitives import hashes, hmac
from cryptography.exceptions import InvalidSignature
import ctypes
import binascii
#specific for OTFDEC 
import numpy

IMAGE_MAGIC = 0x96f3b83d
IMAGE_HEADER_SIZE = 32
BIN_EXT = "bin"
INTEL_HEX_EXT = "hex"
DEFAULT_MAX_SECTORS = 128
MAX_ALIGN = 8
DEP_IMAGES_KEY = "images"
DEP_VERSIONS_KEY = "versions"
MAX_SW_TYPE_LENGTH = 12  # Bytes

# Image header flags.
IMAGE_F = {
        'PIC':                   0x0000001,
        'NON_BOOTABLE':          0x0000010,
        'ENCRYPTED':             0x0000004,
        'OTFDEC':                0x0000008,
        'PRIMARY_ONLY':          0x0000002,
}

c_uint8 = ctypes.c_uint8

class Flags_bits(ctypes.LittleEndianStructure):
    _fields_ = [
            ("pic", c_uint8, 1),
            ("primary_only", c_uint8, 1),
            ("encrypted", c_uint8, 1),
            ("otfdec", c_uint8, 1),
            ("non_bootable", c_uint8, 1),
        ]

class Flags(ctypes.Union):
    _fields_ = [("b", Flags_bits),
                ("asbyte", c_uint8)]
                
TLV_VALUES = {
        'KEYHASH': 0x01,
        'SHA256': 0x10,
        'RSA2048': 0x20,
        'ECDSA224': 0x21,
        'ECDSA256': 0x22,
        'RSA3072': 0x23,
        'ED25519': 0x24,
        'ENCRSA2048': 0x30,
        'ENCKW128': 0x31,
        'ENCEC256': 0x32,
        'DEPENDENCY': 0x40,
        'SEC_CNT': 0x50,
        'BOOT_RECORD': 0x60,
}

TLV_SIZE = 4
TLV_INFO_SIZE = 4
TLV_INFO_MAGIC = 0x6907
TLV_PROT_INFO_MAGIC = 0x6908

boot_magic = bytes([
    0x77, 0xc2, 0x95, 0xf3,
    0x60, 0xd2, 0xef, 0x7f,
    0x35, 0x52, 0x50, 0x0f,
    0x2c, 0xb6, 0x79, 0x80, ])

STRUCT_ENDIAN_DICT = {
        'little': '<',
        'big':    '>'
}

VerifyResult = Enum('VerifyResult',
                    """
                    OK INVALID_MAGIC INVALID_TLV_INFO_MAGIC INVALID_HASH
                    INVALID_SIGNATURE
                    """)


class TLV():
    def __init__(self, endian, magic=TLV_INFO_MAGIC):
        self.magic = magic
        self.buf = bytearray()
        self.endian = endian

    def __len__(self):
        return TLV_INFO_SIZE + len(self.buf)

    def add(self, kind, payload):
        """
        Add a TLV record.  Kind should be a string found in TLV_VALUES above.
        """
        e = STRUCT_ENDIAN_DICT[self.endian]
        buf = struct.pack(e + 'BBH', TLV_VALUES[kind], 0, len(payload))
        self.buf += buf
        self.buf += payload

    def get(self):
        if len(self.buf) == 0:
            return bytes()
        e = STRUCT_ENDIAN_DICT[self.endian]
        header = struct.pack(e + 'HH', self.magic, len(self))
        return header + bytes(self.buf)


class Image():

    def __init__(self, version=None, header_size=IMAGE_HEADER_SIZE,
                 pad_header=False, pad=False, confirm=False, align=1,
                 slot_size=0, max_sectors=DEFAULT_MAX_SECTORS,
                 overwrite_only=False, endian="little", load_addr=0,
                 erased_val=None, save_enctlv=False, security_counter=None, otfdec=None , primary_only=None):
        self.version = version or versmod.decode_version("0")
        self.header_size = header_size
        self.pad_header = pad_header
        self.pad = pad
        self.confirm = confirm
        self.align = align
        self.slot_size = slot_size
        self.max_sectors = max_sectors
        self.overwrite_only = overwrite_only
        self.endian = endian
        self.base_addr = None
        self.load_addr = 0 if load_addr is None else load_addr
        self.erased_val = 0xff if erased_val is None else int(erased_val, 0)
        self.payload = []
        self.enckey = None
        self.address = 0 if otfdec is None else otfdec
        self.flags = 'OTFDEC' if otfdec is not None  else None
        self.primary_only = primary_only      
        self.save_enctlv = save_enctlv
        self.enctlv_len = 0
        if primary_only:
            self.flags = 'PRIMARY_ONLY'
        if security_counter == 'auto':
            # Security counter has not been explicitly provided,
            # generate it from the version number
            self.security_counter = ((self.version.major << 24)
                                     + (self.version.minor << 16)
                                     + self.version.revision)
        else:
            self.security_counter = security_counter

    def __repr__(self):
        return "<Image version={}, header_size={}, security_counter={}, \
                base_addr={}, load_addr={}, align={}, slot_size={}, \
                max_sectors={}, overwrite_only={}, endian={} format={}, \
                payloadlen=0x{:x}>".format(
                    self.version,
                    self.header_size,
                    self.security_counter,
                    self.base_addr if self.base_addr is not None else "N/A",
                    self.load_addr,
                    self.align,
                    self.slot_size,
                    self.max_sectors,
                    self.overwrite_only,
                    self.endian,
                    self.__class__.__name__,
                    len(self.payload))

    def load(self, path):
        """Load an image from a given file"""
        ext = os.path.splitext(path)[1][1:].lower()
        try:
            if ext == INTEL_HEX_EXT:
                ih = IntelHex(path)
                self.payload = ih.tobinarray()
                self.base_addr = ih.minaddr()
            else:
                with open(path, 'rb') as f:
                    self.payload = f.read()
        except FileNotFoundError:
            raise click.UsageError("Input file not found")

        # Add the image header if needed.
        if self.pad_header and self.header_size > 0:
            if self.base_addr:
                # Adjust base_addr for new header
                self.base_addr -= self.header_size
            self.payload = bytes([self.erased_val] * self.header_size) + \
                self.payload

        self.check_header()

    def save(self, path, hex_addr=None):
        """Save an image from a given file"""
        ext = os.path.splitext(path)[1][1:].lower()
        if ext == INTEL_HEX_EXT:
            # input was in binary format, but HEX needs to know the base addr
            if self.base_addr is None and hex_addr is None:
                raise click.UsageError("No address exists in input file "
                                       "neither was it provided by user")
            h = IntelHex()
            if hex_addr is not None:
                self.base_addr = hex_addr
            h.frombytes(bytes=self.payload, offset=self.base_addr)
            if self.pad:
                trailer_size = self._trailer_size(self.align, self.max_sectors,
                                                  self.overwrite_only,
                                                  self.enckey,
                                                  self.save_enctlv,
                                                  self.enctlv_len)
                trailer_addr = (self.base_addr + self.slot_size) - trailer_size
                padding = bytes([self.erased_val] *
                                (trailer_size - len(boot_magic))) + boot_magic
                h.puts(trailer_addr, padding)
            h.tofile(path, 'hex')
        else:
            if self.pad:
                self.pad_to(self.slot_size)
            with open(path, 'wb') as f:
                f.write(self.payload)

    def check_header(self):
        if self.header_size > 0 and not self.pad_header:
            if any(v != 0 for v in self.payload[0:self.header_size]):
                raise click.UsageError("Header padding was not requested and "
                                       "image does not start with zeros")

    def check_trailer(self):
        if self.slot_size > 0:
            tsize = self._trailer_size(self.align, self.max_sectors,
                                       self.overwrite_only, self.enckey,
                                       self.save_enctlv, self.enctlv_len)
            padding = self.slot_size - (len(self.payload) + tsize)
            if padding < 0:
                msg = "Image size (0x{:x}) + trailer (0x{:x}) exceeds " \
                      "requested size 0x{:x}".format(
                          len(self.payload), tsize, self.slot_size)
                raise click.UsageError(msg)

    def ecies_p256_hkdf(self, enckey, plainkey):
        newpk = ec.generate_private_key(ec.SECP256R1(), default_backend())
        shared = newpk.exchange(ec.ECDH(), enckey._get_public())
        derived_key = HKDF(
            algorithm=hashes.SHA256(), length=48, salt=None,
            info=b'MCUBoot_ECIES_v1', backend=default_backend()).derive(shared)
        encryptor = Cipher(algorithms.AES(derived_key[:16]),
                           modes.CTR(bytes([0] * 16)),
                           backend=default_backend()).encryptor()
        cipherkey = encryptor.update(plainkey) + encryptor.finalize()
        mac = hmac.HMAC(derived_key[16:], hashes.SHA256(),
                        backend=default_backend())
        mac.update(cipherkey)
        ciphermac = mac.finalize()
        pubk = newpk.public_key().public_bytes(
            encoding=Encoding.X962,
            format=PublicFormat.UncompressedPoint)
        return cipherkey, ciphermac, pubk

    def create(self, key, enckey, dependencies=None, sw_type=None, clear=False):
        self.enckey = enckey

        # Calculate the hash of the public key
        if key is not None:
            pub = key.get_public_bytes()
            sha = hashlib.sha256()
            sha.update(pub)
            pubbytes = sha.digest()
        else:
            pubbytes = bytes(hashlib.sha256().digest_size)
        protected_tlv_size = 0

        if self.security_counter is not None:
            # Size of the security counter TLV: header ('HH') + payload ('I')
            #                                   = 4 + 4 = 8 Bytes
            protected_tlv_size += TLV_SIZE + 4

        if sw_type is not None:
            if len(sw_type) > MAX_SW_TYPE_LENGTH:
                msg = "'{}' is too long ({} characters) for sw_type. Its " \
                      "maximum allowed length is 12 characters.".format(
                       sw_type, len(sw_type))
                raise click.UsageError(msg)
            image_version = (str(self.version.major) + '.'
                             + str(self.version.minor) + '.'
                             + str(self.version.revision))

            # The image hash is computed over the image header, the image
            # itself and the protected TLV area. However, the boot record TLV
            # (which is part of the protected area) should contain this hash
            # before it is even calculated. For this reason the script fills
            # this field with zeros and the bootloader will insert the right
            # value later.
            digest = bytes(hashlib.sha256().digest_size)

            # Create CBOR encoded boot record
            boot_record = create_sw_component_data(sw_type, image_version,
                                                   "SHA256", digest,
                                                   pubbytes)

            protected_tlv_size += TLV_SIZE + len(boot_record)

        if dependencies is not None:
            # Size of a Dependency TLV = Header ('HH') + Payload('IBBHI')
            # = 4 + 12 = 16 Bytes
            dependencies_num = len(dependencies[DEP_IMAGES_KEY])
            protected_tlv_size += (dependencies_num * 16)

        if protected_tlv_size != 0:
            # Add the size of the TLV info header
            protected_tlv_size += TLV_INFO_SIZE

        # At this point the image is already on the payload, this adds
        # the header to the payload as well
        self.add_header(enckey, protected_tlv_size)

        prot_tlv = TLV(self.endian, TLV_PROT_INFO_MAGIC)

        # Protected TLVs must be added first, because they are also included
        # in the hash calculation
        protected_tlv_off = None
        if protected_tlv_size != 0:

            e = STRUCT_ENDIAN_DICT[self.endian]

            if self.security_counter is not None:
                payload = struct.pack(e + 'I', self.security_counter)
                prot_tlv.add('SEC_CNT', payload)

            if sw_type is not None:
                prot_tlv.add('BOOT_RECORD', boot_record)

            if dependencies is not None:
                for i in range(dependencies_num):
                    payload = struct.pack(
                                    e + 'B3x'+'BBHI',
                                    int(dependencies[DEP_IMAGES_KEY][i]),
                                    dependencies[DEP_VERSIONS_KEY][i].major,
                                    dependencies[DEP_VERSIONS_KEY][i].minor,
                                    dependencies[DEP_VERSIONS_KEY][i].revision,
                                    dependencies[DEP_VERSIONS_KEY][i].build
                                    )
                    prot_tlv.add('DEPENDENCY', payload)

            protected_tlv_off = len(self.payload)
            self.payload += prot_tlv.get()

        tlv = TLV(self.endian)

        # Note that ecdsa wants to do the hashing itself, which means
        # we get to hash it twice.
        if self.flags != 'OTFDEC':
            sha = hashlib.sha256()
            sha.update(self.payload)
            digest = sha.digest()
            tlv.add('SHA256', digest)
            print("digest"+str(binascii.hexlify(digest)))
            if key is not None:
                tlv.add('KEYHASH', pubbytes)
                # `sign` expects the full image payload (sha256 done internally),
                # while `sign_digest` expects only the digest of the payload

                if hasattr(key, 'sign'):
                    sig = key.sign(bytes(self.payload))
                else:
                    sig = key.sign_digest(digest)
                tlv.add(key.sig_tlv(), sig)

        # At this point the image was hashed + signed, we can remove the
        # protected TLVs from the payload (will be re-added later)
        if protected_tlv_off is not None:
            self.payload = self.payload[:protected_tlv_off]

        if enckey is not None:
            plainkey = os.urandom(16)
            #value_key=0
            #plainkey = value_key.to_bytes(16, byteorder = 'little') 
            print("aes key value"+bytes(plainkey).hex())
            if isinstance(enckey, RSAPublic):
                cipherkey = enckey._get_public().encrypt(
                    plainkey, padding.OAEP(
                        mgf=padding.MGF1(algorithm=hashes.SHA256()),
                        algorithm=hashes.SHA256(),
                        label=None))
                self.enctlv_len = len(cipherkey)
                tlv.add('ENCRSA2048', cipherkey)
            elif isinstance(enckey, ECDSA256P1Public):
                cipherkey, mac, pubk = self.ecies_p256_hkdf(enckey, plainkey)
                enctlv = pubk + mac + cipherkey
                self.enctlv_len = len(enctlv)
                tlv.add('ENCEC256', enctlv)

            address = int(self.address / 16)
            #address_other = 0
            #nonce_other = address_other.to_bytes(8, byteorder = 'little') 
            nonce = address.to_bytes(16, byteorder = 'big')        
            #print(nonce_other)
            #perform 2 encryptor 
            #encryptor_other = AES.new(plainkey, AES.MODE_CTR, nonce= nonce_other, initial_value=address);
            cipher = Cipher(algorithms.AES(plainkey), modes.CTR(nonce),
                           backend=default_backend())
            encryptor = cipher.encryptor()
            img = bytes(self.payload[self.header_size:])
            if self.flags == 'OTFDEC':
                #add bytes to reach 16 bytes
                print("initial len img"+hex(len(img)))
                toadd = (16-len(img)%16)
                img += toadd*b'0'
                #Swap bytes inside 16 bytes block for OTFDEC
                inarr = numpy.asarray(list(img), numpy.int8).reshape(-1, 16)
                outarr = numpy.fliplr(inarr)
                img = bytearray(outarr)
                #print("modified len img"+hex(len(img)))
            #encrypt image 
            #img_other = img
            #encrypted =  encryptor_other.encrypt(img_other)
            #nonce_other = encryptor_other.nonce
            #print("nonce encryptor"+bytes(nonce_other).hex())
            #print("encrypted "+bytes(encrypted[:16]).hex())
            img = encryptor.update(img) + encryptor.finalize()
            #print("img "+bytes(img[:16]).hex())
            #img=encrypted
            #print("img "+bytes(img[:16]).hex())
            if self.flags == 'OTFDEC':
                #Swap bytes inside 16 bytes block for OTFDEC
                inarr = numpy.asarray(list(img), numpy.int8).reshape(-1, 16)
                outarr = numpy.fliplr(inarr)
                img = bytearray(outarr)
                #print("modify len img"+hex(len(img)))
                img = img[:-toadd]   
                #print("final len img"+hex(len(img)))
            #update image , to build image non encrypted with a key in header to allow swap, an option is 
            #required their, this option
            if clear == False:
                self.payload[self.header_size:] = img 
            if self.flags == 'OTFDEC':
                # add the protected TLV 
                self.payload += prot_tlv.get()
                sha = hashlib.sha256()
                sha.update(self.payload)
                digest = sha.digest()
                tlv.add('SHA256', digest)
                #print("otfdec digest"+str(binascii.hexlify(digest)))
                if key is not None:
                    tlv.add('KEYHASH', pubbytes)
                    # `sign` expects the full image payload (sha256 done internally),
                    # while `sign_digest` expects only the digest of the payload

                    if hasattr(key, 'sign'):
                        sig = key.sign(bytes(self.payload))
                    else:
                        sig = key.sign_digest(digest)              
                    tlv.add(key.sig_tlv(), sig)
        if self.flags == 'PRIMARY_ONLY' and enckey is not None:
            self.add_header(enckey, protected_tlv_size, True)    
        if self.flags != 'OTFDEC':
            self.payload += prot_tlv.get()
        #add encrypted flag if image has been encrypted        
        self.payload += tlv.get()
        self.check_trailer()

    def add_header(self, enckey, protected_tlv_size, force_encrypted=False ):
        """Install the image header."""

        flags = 0
        if enckey is not None:
            #encrypted primary image only has hash computed on a header without flag encrypted.
            if self.flags != 'PRIMARY_ONLY' and self.flags != 'OTFDEC':
                flags |= IMAGE_F['ENCRYPTED']
            if force_encrypted:
                flags |= IMAGE_F['ENCRYPTED']
        #all specific images are flags to be able to discrimate images
        if self.flags is not None:
            flags |= IMAGE_F[self.flags]              
        e = STRUCT_ENDIAN_DICT[self.endian]
        fmt = (e +
               # type ImageHdr struct {
               'I' +     # Magic    uint32
               'I' +     # LoadAddr uint32
               'H' +     # HdrSz    uint16
               'H' +     # PTLVSz   uint16
               'I' +     # ImgSz    uint32
               'I' +     # Flags    uint32
               'BBHI' +  # Vers     ImageVersion
               'I'       # Pad1     uint32
               )  # }
        assert struct.calcsize(fmt) == IMAGE_HEADER_SIZE
        header = struct.pack(fmt,
                IMAGE_MAGIC,
                self.load_addr,
                self.header_size,
                protected_tlv_size,  # TLV Info header + Protected TLVs
                len(self.payload) - self.header_size,  # ImageSz
                flags,
                self.version.major,
                self.version.minor or 0,
                self.version.revision or 0,
                self.version.build or 0,
                0)  # Pad1
        self.payload = bytearray(self.payload)
        self.payload[:len(header)] = header

    def _trailer_size(self, write_size, max_sectors, overwrite_only, enckey,
                      save_enctlv, enctlv_len):
        # NOTE: should already be checked by the argument parser
        magic_size = 16
        if overwrite_only:
            return MAX_ALIGN * 2 + magic_size
        else:
            if write_size not in set([1, 2, 4, 8,16]):
                raise click.BadParameter("Invalid alignment: {}".format(
                    write_size))
            m = DEFAULT_MAX_SECTORS if max_sectors is None else max_sectors
            trailer = m * 3 * write_size  # status area
            if enckey is not None:
                if save_enctlv:
                    # TLV saved by the bootloader is aligned
                    keylen = (int((enctlv_len - 1) / MAX_ALIGN) + 1) * MAX_ALIGN
                else:
                    keylen = 16
                trailer += keylen * 2  # encryption keys
            trailer += MAX_ALIGN * 4  # image_ok/copy_done/swap_info/swap_size
            trailer += magic_size
            return trailer

    def pad_to(self, size):
        """Pad the image to the given size, with the given flash alignment."""
        tsize = self._trailer_size(self.align, self.max_sectors,
                                   self.overwrite_only, self.enckey,
                                   self.save_enctlv, self.enctlv_len)
        padding = size - (len(self.payload) + tsize)
        pbytes = bytearray([self.erased_val] * padding)
        pbytes += bytearray([self.erased_val] * (tsize - len(boot_magic)))
        if self.confirm and not self.overwrite_only:
            pbytes[-MAX_ALIGN] = 0x01  # image_ok = 0x01
        pbytes += boot_magic
        self.payload += pbytes

    @staticmethod
    def verify(imgfile, key):
        with open(imgfile, "rb") as f:
            b = f.read()
    # Magic    uint32
    # LoadAddr uint32
    # HdrSz    uint16
    # PTLVSz   uint16
    # ImgSz    uint32
    # Flags    uint32
    # Vers :    
    # iv_major uint8;
    # iv_minor uint8;
    # iv_revision uint16;
    # iv_build_num uint32;
        magic, Load_addr, header_size, ptlvs, img_size, flags = struct.unpack('IIHHII', b[:20])
        version = struct.unpack('BBHI', b[20:28])
        iv_major, iv_minor, iv_revision, iv_build_num = struct.unpack('BBHI', b[20:28])
        print("################ HEADER #################")
        print("Magic "+hex(magic))
        print("LoadAddr "+hex(Load_addr))
        print("HdrSz "+hex(header_size))
        print("PTLVSz "+hex(ptlvs))
        print("ImgSz "+hex(img_size))
        print("Flags "+hex(flags))
        print("Version :  M="+str(iv_major)+", m="+str(iv_minor)+", r="+str(iv_revision)+", b="+hex(iv_build_num))
        if magic != IMAGE_MAGIC:
            return VerifyResult.INVALID_MAGIC, None
        decodedflags = Flags()
        decodedflags.asbyte = flags
        print("Header Flags"+decodedflags.b.primary_only*" PRIMARY_ONLY"+decodedflags.b.encrypted*" ENCRYPTED"++decodedflags.b.otfdec*" OTFDEC")
        print("################ TLV protected #################")
        tlv_info = b[header_size+img_size:header_size+img_size+TLV_INFO_SIZE]
        magic, tlv_tot = struct.unpack('HH', tlv_info)
        print("TLV Magic "+hex(magic)+(magic==TLV_PROT_INFO_MAGIC)*" PROTECTED TLV"+(magic==TLV_INFO_MAGIC)*" NON_PROTECTED TLV"+" len "+hex(tlv_tot))
        print("TLV prot begin offset = "+hex(header_size+img_size+TLV_INFO_SIZE))
        print("TLV prot end offset =  "+hex(header_size+img_size+ ptlvs)+" "+hex(header_size+img_size+tlv_tot))
        tlv_off = header_size+img_size
        tlv_end = header_size+img_size + tlv_tot
        tlv_off += TLV_INFO_SIZE  # skip tlv info
        while tlv_off < tlv_end:
            tlv = b[tlv_off:tlv_off+TLV_SIZE]
            tlv_type, _, tlv_len = struct.unpack('BBH', tlv)
            #get TLV string value 
            for tlv_string, index in TLV_VALUES.items():
                if tlv_type == index:
                    tlv_decoded=tlv_string
            tlv_value = b[tlv_off+TLV_SIZE:tlv_off+TLV_SIZE+tlv_len]
            print("TLV "+hex(tlv_type)+" "+tlv_decoded+" "+str(binascii.hexlify(tlv_value))) 
            tlv_off += TLV_SIZE + tlv_len
        print("############ TLV Non protected  ###########")
        tlv_nonprotected_off = header_size+img_size+ ptlvs
        tlv_info = b[header_size+img_size+tlv_tot:header_size+img_size+tlv_tot+TLV_INFO_SIZE]
        magic, tlv_tot = struct.unpack('HH', tlv_info)
        print("TLV Magic "+hex(magic)+(magic==TLV_PROT_INFO_MAGIC)*" PROTECTED TLV"+(magic==TLV_INFO_MAGIC)*" NON_PROTECTED TLV"+" len "+hex(tlv_tot))
        
        #compute sha256 of image and protected TLV
        sha = hashlib.sha256()
        sha.update(b[:header_size+img_size+ptlvs])
        digest = sha.digest()
        #print("digest"+str(binascii.hexlify(digest)))
        #digest_string = " ".join(hex(ord(n)) for n in digest)
        #print(digest_string)
        #parse TLV to display
        tlv_off = tlv_nonprotected_off
        tlv_end = tlv_nonprotected_off + tlv_tot
        tlv_off += TLV_INFO_SIZE  # skip tlv info
        #Parse TLV to display
        while tlv_off < tlv_end:
            tlv = b[tlv_off:tlv_off+TLV_SIZE]
            tlv_type, _, tlv_len = struct.unpack('BBH', tlv)
            #get TLV string value 
            for tlv_string, index in TLV_VALUES.items():
                if tlv_type == index: 
                    tlv_decoded=tlv_string
            tlv_value = b[tlv_off+TLV_SIZE:tlv_off+TLV_SIZE+tlv_len]
            print("TLV "+hex(tlv_type)+"("+hex(tlv_len)+")"+tlv_decoded+" "+str(binascii.hexlify(tlv_value)))
            tlv_off += TLV_SIZE + tlv_len
        #Parse TLV to validate signature
        tlv_off = tlv_nonprotected_off
        tlv_end = tlv_nonprotected_off + tlv_tot
        tlv_off += TLV_INFO_SIZE  # skip tlv info
        while tlv_off < tlv_end:
            tlv = b[tlv_off:tlv_off+TLV_SIZE]
            tlv_type, _, tlv_len = struct.unpack('BBH', tlv)
            #get TLV string value 
            for tlv_string, index in TLV_VALUES.items():
                if tlv_type == index: 
                    tlv_decoded=tlv_string 
            if tlv_type == TLV_VALUES["SHA256"]:
                off = tlv_off + TLV_SIZE
                if digest == b[off:off+tlv_len]:
                    print("Hash Validated")
                    if key is None:
                        return VerifyResult.OK, version
                else:
                    if decodedflags.b.encrypted and not decodedflags.b.otfdec:
                        print("Hash verification not supported")
                    else:
                        return VerifyResult.INVALID_HASH, None
            elif key is not None and tlv_type == TLV_VALUES[key.sig_tlv()]:
                off = tlv_off + TLV_SIZE
                tlv_sig = b[off:off+tlv_len]
                payload = b[:header_size+img_size+ptlvs]               
                try:
                    if hasattr(key, 'verify'):
                        key.verify(tlv_sig, payload)
                    print("Signature Validated")
                    return VerifyResult.OK, version
                except InvalidSignature:
                    # continue to next TLV
                    pass
            tlv_off += TLV_SIZE + tlv_len
        if decodedflags.b.primary_only and decodedflags.b.encrypted :
            return VerifyResult.OK, version
        if decodedflags.b.encrypted and not decodedflags.b.otfdec:
            print("Signature verification not supported")
            return VerifyResult.OK, version        
        return VerifyResult.INVALID_SIGNATURE, None
