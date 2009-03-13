package com.parc.ccn.network.daemons.repo;

import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;

import javax.xml.namespace.QName;
import javax.xml.stream.XMLEventReader;
import javax.xml.stream.XMLInputFactory;
import javax.xml.stream.XMLStreamException;
import javax.xml.stream.events.Attribute;
import javax.xml.stream.events.XMLEvent;

import com.parc.ccn.data.ContentName;
import com.parc.ccn.data.ContentObject;
import com.parc.ccn.data.MalformedContentNameStringException;
import com.parc.ccn.library.CCNLibrary;

/**
 * 
 * @author rasmusse
 *
 */
public class BasicPolicy implements Policy {
	
	public static final String POLICY = "POLICY";
	
	private String _version = null;
	private byte [] _content = null;
	private String _globalName = null;
	private String _localName = null;
	private boolean _localNameMatched = false;
	private boolean _globalNameMatched = false;
	
	public BasicPolicy(String name) {
		this._localName = name;
	}
	
	private enum PolicyValue {
		VERSION ("VERSION"),
		NAMESPACE ("NAMESPACE"),
		GLOBALNAME ("GLOBALNAME"),
		LOCALNAME ("LOCALNAME"),
		UNKNOWN();
		
		private String _stringValue = null;
		
		PolicyValue() {}
		
		PolicyValue(String stringValue) {
			this._stringValue = stringValue;
		}
		
		static PolicyValue valueFromString(String value) {
			for (PolicyValue pv : PolicyValue.values()) {
				if (pv._stringValue != null) {
					if (pv._stringValue.equals(value.toUpperCase()))
						return pv;
				}
			}
			return UNKNOWN;
		}
	}
	
	private ArrayList<ContentName> nameSpace = new ArrayList<ContentName>(0);

	public boolean update(InputStream stream, boolean fromNet) throws XMLStreamException, IOException {
		_content = new byte[stream.available()];
		stream.read(_content);
		stream.close();
		ByteArrayInputStream bais = new ByteArrayInputStream(_content);
		XMLInputFactory factory = XMLInputFactory.newInstance();
		XMLEventReader reader = factory.createXMLEventReader(bais);
		XMLEvent event = reader.nextEvent();
		_version = null;
		_localNameMatched = false;
		_globalNameMatched = false;
		if (!event.isStartDocument()) {
			throw new XMLStreamException("Expected start document, got: " + event.toString());
		}
		try {
			parseXML(reader, null, null, POLICY, false, fromNet);
		} catch (RepositoryException e) {
			return false; // wrong hostname - i.e. not for us
		}
		reader.close();
		if (_version == null)
			throw new XMLStreamException("No version in policy file");
		if (!_localNameMatched)
			throw new XMLStreamException("No local name in policy file");
		if (!_globalNameMatched)
			throw new XMLStreamException("No global name in policy file");
		return true;
	}

	public ArrayList<ContentName> getNameSpace() {
		return nameSpace;
	}
	
	/**
	 * For now we only expect the values "policy", "version", and "namespace"
	 * @param reader
	 * @param expectedValue
	 * @return
	 * @throws XMLStreamException
	 * @throws RepositoryException 
	 */
	private XMLEvent parseXML(XMLEventReader reader, XMLEvent event, String value, String expectedValue, boolean started,
				boolean fromNet) 
				throws XMLStreamException, RepositoryException {
		if (started) {
			switch (PolicyValue.valueFromString(value)) {
			case VERSION:
				QName id = new QName("id");
				Attribute idAttr = event.asStartElement().getAttributeByName(id);
				if (idAttr != null) {
					if (!idAttr.getValue().trim().equals("1.0"))
						throw new XMLStreamException("Bad version in policy file");
					_version = value;
				}
				break;
			default:
				break;
			}
		}
		
		event = reader.nextEvent();
		boolean finished = false;
		while (!finished) {
			if (event.isStartElement()) {
				String startValue = event.asStartElement().getName().toString();
				if (expectedValue != null) {
					if (!startValue.toUpperCase().equals(expectedValue.toUpperCase()))
						throw new XMLStreamException("Expected " + expectedValue + ", got: " + value);
					event = reader.nextEvent();
					value = expectedValue;
					expectedValue = null;
				} else {
					event = parseXML(reader, event, startValue, null, true, fromNet);
				}
			} else if (event.isEndElement()) {
				String newValue = event.asEndElement().getName().toString();
				if (!newValue.toUpperCase().equals(value.toUpperCase()))
					throw new XMLStreamException("Expected end of " + value + ", got: " + newValue);
				event = reader.nextEvent();
				finished = true;
			} else if (event.isCharacters()) {
				if (started) {
					switch (PolicyValue.valueFromString(value)) {
					case NAMESPACE:
						String charValue = event.asCharacters().getData();
						try {
							nameSpace.add(ContentName.fromNative(charValue.trim()));
						} catch (MalformedContentNameStringException e) {
							throw new XMLStreamException("Malformed value in namespace: " + charValue);
						}
						break;
					case LOCALNAME:
						charValue = event.asCharacters().getData();
						String localName = charValue.trim();
						if (fromNet) {
							if (!checkMatch(localName, _localName))
								throw new RepositoryException("Repository local name doesn't match");
						} else
							_localName = localName;
						_localNameMatched = true;
						break;
					case GLOBALNAME:
						charValue = event.asCharacters().getData();
						String globalName = charValue.trim();
						if (fromNet) {
							if (!checkMatch(globalName, _globalName))
								throw new RepositoryException("Repository global name doesn't match");
						} else {
							if (!globalName.startsWith("/"))
								globalName = "/" + globalName;
							_globalName = globalName;
						}
						_globalNameMatched = true;
						break;
					default:
						break;
					}
				}
				event = reader.nextEvent();
			} else if (event.isEndDocument()) {
				finished = true;
			}
		}
		return event;
	}

	public ContentObject getPolicyContent() {
		try {
			return CCNLibrary.getContent(ContentName.fromNative(_globalName + "/" + _localName +
					"/" + Repository.REPO_DATA + "/" + Repository.REPO_POLICY), 
					_content);
		} catch (MalformedContentNameStringException e) {return null;}	// shouldn't happen
	}
	
	private boolean checkMatch(String name, String matchName) {
		if (matchName == null)
			return true;
		if (name.equals(matchName))
			return true;
		return false;
	}
}