/**
 * Part of the CCNx Java Library.
 *
 * Copyright (C) 2008, 2009 Palo Alto Research Center, Inc.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1
 * as published by the Free Software Foundation. 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details. You should have received
 * a copy of the GNU Lesser General Public License along with this library;
 * if not, write to the Free Software Foundation, Inc., 51 Franklin Street,
 * Fifth Floor, Boston, MA 02110-1301 USA.
 */
package org.ccnx.ccn.profiles.security;

import java.io.IOException;

import org.ccnx.ccn.profiles.CCNProfile;
import org.ccnx.ccn.profiles.CommandMarker;
import org.ccnx.ccn.protocol.ContentName;
import org.ccnx.ccn.protocol.PublisherPublicKeyDigest;

/**
 * The Key profile handles low-level functions regarding representing
 * keys in content and content names. This allows us to provide a standard
 * form for referring to public keys in names, among other things.
 */
public class KeyProfile implements CCNProfile {
	
	public static final byte [] KEY_NAME_COMPONENT = ContentName.componentParseNative("KEYS");
	public static final CommandMarker KEY_ID_PREFIX = 
		CommandMarker.commandMarker(CommandMarker.MARKER_NAMESPACE, "K");
	
	/**
	 * This builds a name component which refers to the digest
	 * of a key (of any type), as generated by the caller,
	 * formatted in a standard way (e.g. marker prefixes if
	 * necessary). This makes it easier to write code that
	 * writes and parses names with key identifiers as 
	 * name components.
	 * @param keyID The (digest) identifier of the key to
	 * 	be referred to.
	 * @return The resulting name component.
	 */
	public static byte [] keyIDToNameComponent(byte [] keyID) {
		
		if (null == keyID) {
			// for now, don't complain about 0-length ID
			throw new IllegalArgumentException("keyID must not be null!");
		}
		
		return KEY_ID_PREFIX.addBinaryData(keyID);
	}
	
	/**
	 * Helper method to return key ID name component as a string.
	 */
	public static String keyIDToNameComponentAsString(PublisherPublicKeyDigest keyID) {
		return ContentName.componentPrintURI(keyIDToNameComponent(keyID));
	}
	
	/**
	 * This generates a name component which refers to the digest of a
	 * public key, formatted in a standard way (e.g. with marker prefixes
	 * if necessary and so on).
	 * @param keyToName The key to include in the name component.
	 * @return the binary name component
	 */
	public static byte [] keyIDToNameComponent(PublisherPublicKeyDigest keyToName) {
	
		if (null == keyToName) {
			throw new IllegalArgumentException("keyToName must not be null!");
		}
		
		return keyIDToNameComponent(keyToName.digest());
	}
	
	/**
	 * This creates a ContentName whose last component represents
	 * the digest of a public key.
	 * @param parent the parent (prefix) to use for this content name;
	 * 	if null, the name will contain only the key ID component.
	 * @param keyToName the key to refer to in the next name component.
	 * @return the resulting name
	 */
	public static ContentName keyName(ContentName parent, PublisherPublicKeyDigest keyToName) {	
		return new ContentName(parent, keyIDToNameComponent(keyToName));
	}
	
	/**
	 * This creates a ContentName whose last component represents
	 * the digest of a key.
	 * @param parent the parent (prefix) to use for this content name;
	 * 	if null, the name will contain only the key ID component.
	 * @param keyID the key ID to refer to in the next name component.
	 * @return the resulting name
	 */
	public static ContentName keyName(ContentName parent, byte [] keyID) {	
		return new ContentName(parent, keyIDToNameComponent(keyID));
	}

	/**
	 * Get the target keyID from a name component.
	 * Wrapped key blocks are stored under a name whose last (pre content digest) component
	 * identifies the key used to wrap them, as 
	 * WRAPPING_KEY_PREFIX COMPONENT_SEPARATOR base64Encode(keyID)
	 * or 
	 * keyid:<base 64 encoding of binary key id>
	 * The reason for the prefix is to allow unique separation from the principal name
	 * links, the reason for the base 64 encoding is to allow unique separation from the
	 * prefix.
	 * @param childName the name component
	 * @return the keyID
	 * @throws IOException
	 */
	public static byte[] getKeyIDFromNameComponent(byte[] childName) throws IOException {
		if (!KeyProfile.isKeyNameComponent(childName))
			return null;
		byte [] keyid = CommandMarker.extractApplicationData(childName);
		return keyid;
	}

	/**
	 * Returns whether a specified name component is the name of a wrapped key
	 * @param wnkNameComponent the name component
	 * @return
	 */
	public static boolean isKeyNameComponent(byte [] wnkNameComponent) {
		return KEY_ID_PREFIX.isMarker(wnkNameComponent);
	}
}
