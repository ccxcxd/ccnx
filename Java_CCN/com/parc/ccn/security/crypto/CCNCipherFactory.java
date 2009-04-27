package com.parc.ccn.security.crypto;

import java.math.BigInteger;
import java.security.InvalidAlgorithmParameterException;
import java.security.InvalidKeyException;
import java.security.NoSuchAlgorithmException;

import javax.crypto.Cipher;
import javax.crypto.NoSuchPaddingException;
import javax.crypto.spec.IvParameterSpec;
import javax.crypto.spec.SecretKeySpec;

import com.parc.ccn.Library;

/**
 * Static factory methods for initializing CipherInputStream and CipherOutputStreams
 * for decrypting and encrypting CCN segment-level data.
 * 
 * Intended to be used internally by the segmenter only.
 * 
 * @author smetters
 *
 */
public class CCNCipherFactory {
	
	/**
	 * The core encryption algorithms supported. Any native encryption
	 * mode supported by Java *should* work, but these are compactly
	 * encodable.
	 */
	public static final String AES_CTR_MODE = "AES/CTR/NoPadding";
	public static final String AES_CBC_MODE = "AES/CBC/PKCS5Padding";
	public static final String DEFAULT_CIPHER_ALGORITHM = AES_CTR_MODE;
	
	public static final int IV_MASTER_LENGTH = 8; // bytes
	public static final int SEGMENT_NUMBER_LENGTH = 6; // bytes
	public static final int BLOCK_COUNTER_LENGTH = 2; // bytes
	private static final byte [] INITIAL_BLOCK_COUNTER_VALUE = new byte[]{0x00, 0x01};

	/**
	 * Make an encrypting Cipher to be used in making a CipherOutputStream to
	 * wrap outgoing CCN data.
	 * 
	 * This will use the CCN defaults for IV handling, to ensure that segments
	 * of a given larger piece of content do not have overlapping key streams.
	 * Higher-level functionality embodied in the library (or application-specific
	 * code) should be used to make sure that the key, masterIV pair used for a 
	 * given multi-block piece of content is unique for that content.
	 * 
	 * CCN encryption algorithms assume deterministic IV generation (e.g. from 
	 * cryptographic MAC or ciphers themselves), and therefore do not transport
	 * the IV explicitly. Applications that wish to do so need to arrange
	 * IV transport.
	 * 
	 * We assume this stream starts on the first block of a multi-block segement,
	 * so for CTR mode, the initial block counter is 1 (block ==  encryption
	 * block). (Conventions for counter start them at 1, not 0.) The cipher
	 * will automatically increment the counter; if it overflows the two bytes
	 * we've given to it it will start to increment into the segment number.
	 * This runs the risk of potentially using up some of the IV space of
	 * other segments. 
	 * 
	 * CTR_init = IV_master || segment_number || block_counter
	 * CBC_iv = E_Ko(IV_master || segment_number || 0x0001)
	 * 		(just to make it easier, use the same feed value)
	 * 
	 * CTR value is 16 bytes.
	 * 		8 bytes are the IV.
	 * 		6 bytes are the segment number.
	 * 		last 2 bytes are the block number (for 16 byte blocks); if you 
	 * 	    have more space, use it for the block counter.
	 * IV value is the block width of the cipher.
	 * 
	 * 
	 * @throws InvalidAlgorithmParameterException 
	 * @throws InvalidKeyException 
	 * @throws NoSuchPaddingException 
	 * @throws NoSuchAlgorithmException 
	 */
	public static Cipher getSegmentEncryptionCipher(
			Cipher optionalExistingCipherToInitialize,
			String encAlgorithm, 
			SecretKeySpec secretKey,
			IvParameterSpec masterIV, 
			long segmentNumber) 
	throws InvalidKeyException, InvalidAlgorithmParameterException, 
	NoSuchAlgorithmException, NoSuchPaddingException {

		// Should we handle autogenerated IVs? (If you pass in null to the cipher, if it
		// needs an IV it will make one, which you can ask it for and transfer to the
		// other end somehow.
		// DKS -- TODO eventually.
		if (((null == encAlgorithm) && (null == optionalExistingCipherToInitialize)) || (null == secretKey) || (null == masterIV)) {
			throw new InvalidAlgorithmParameterException("Algorithm (or cipher), key and IV cannot be null!");
		}
		
		Cipher cipher = null;
		if ((null != optionalExistingCipherToInitialize) && 
					(optionalExistingCipherToInitialize.getAlgorithm().equals(encAlgorithm))) {
			cipher = optionalExistingCipherToInitialize;
		} else {
			cipher = Cipher.getInstance(encAlgorithm);
		}
		
		// Construct the IV/initial counter.
		if (0 == cipher.getBlockSize()) {
			Library.logger().warning(encAlgorithm + " is not a block cipher!");
			throw new InvalidAlgorithmParameterException(encAlgorithm + " is not a block cipher!");
		}
		
		if (masterIV.getIV().length < IV_MASTER_LENGTH) {
			throw new InvalidAlgorithmParameterException("Master IV length must be at least " + IV_MASTER_LENGTH + " bytes, it is: " + masterIV.getIV().length);			
		}
		
		IvParameterSpec iv_ctrSpec = buildIVCtr(masterIV, segmentNumber, cipher.getBlockSize());
		cipher.init(Cipher.ENCRYPT_MODE, secretKey, iv_ctrSpec);
		
		return cipher;
	}
	
	
	public static Cipher getSegmentDecryptionCipher(
			Cipher optionalExistingCipherToInitialize,
			String encAlgorithm, 
			SecretKeySpec secretKey,
			IvParameterSpec masterIV, 
			long segmentNumber) 
	throws InvalidKeyException, InvalidAlgorithmParameterException, 
	NoSuchAlgorithmException, NoSuchPaddingException {

		// Should we handle autogenerated IVs? (If you pass in null to the cipher, if it
		// needs an IV it will make one, which you can ask it for and transfer to the
		// other end somehow.
		// DKS -- TODO eventually.
		if (((null == encAlgorithm) && (null == optionalExistingCipherToInitialize)) || (null == secretKey) || (null == masterIV)) {
			throw new InvalidAlgorithmParameterException("Algorithm (or cipher), key and IV cannot be null!");
		}
		
		Cipher cipher = null;
		if ((null != optionalExistingCipherToInitialize) && 
					(optionalExistingCipherToInitialize.getAlgorithm().equals(encAlgorithm))) {
			cipher = optionalExistingCipherToInitialize;
		} else {
			cipher = Cipher.getInstance(encAlgorithm);
		}
		
		// Construct the IV/initial counter.
		if (0 == cipher.getBlockSize()) {
			Library.logger().warning(encAlgorithm + " is not a block cipher!");
			throw new InvalidAlgorithmParameterException(encAlgorithm + " is not a block cipher!");
		}
		
		if (masterIV.getIV().length < IV_MASTER_LENGTH) {
			throw new InvalidAlgorithmParameterException("Master IV length must be at least " + IV_MASTER_LENGTH + " bytes, it is: " + masterIV.getIV().length);			
		}
		
		IvParameterSpec iv_ctrSpec = buildIVCtr(masterIV, segmentNumber, cipher.getBlockSize());
		cipher.init(Cipher.DECRYPT_MODE, secretKey, iv_ctrSpec);
		
		return cipher;
	}
	
	public static IvParameterSpec buildIVCtr(IvParameterSpec masterIV, long segmentNumber, int ivLen) {
		
		byte [] iv_ctr = new byte[ivLen];
		
		System.arraycopy(masterIV.getIV(), 0, iv_ctr, 0, IV_MASTER_LENGTH);
		byte [] byteSegNum = segmentNumberToByteArray(segmentNumber);
		System.arraycopy(byteSegNum, 0, iv_ctr, IV_MASTER_LENGTH, byteSegNum.length);
		System.arraycopy(INITIAL_BLOCK_COUNTER_VALUE, 0, iv_ctr, iv_ctr.length - BLOCK_COUNTER_LENGTH, BLOCK_COUNTER_LENGTH);
		
		IvParameterSpec iv_ctrSpec = new IvParameterSpec(iv_ctr);
		return iv_ctrSpec;
	}
	
	public static byte [] segmentNumberToByteArray(long segmentNumber) {
		byte [] ba = new byte[SEGMENT_NUMBER_LENGTH];
		// Is this the fastest way to do this?
		byte [] bv = BigInteger.valueOf(SEGMENT_NUMBER_LENGTH).toByteArray();
		System.arraycopy(bv, 0, ba, SEGMENT_NUMBER_LENGTH-bv.length, bv.length);
		return ba;
	}
	
	/**
	 * You don't actually want to make one of these.
	 */
	private CCNCipherFactory() {}
}
