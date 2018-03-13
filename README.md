File format
===========
```text
AuFile          = HeaderRecord, {NonHeaderRecord};

HeaderRecord    = 'H', AuVersion, EndOfRecord;
ClearDictRecord = 'C', EndOfRecord;
AddDictRecord   = 'A', RelDictLocation, {StringValue}, EndOfRecord;
ValueRecord     = 'V', RelDictLocation, ValueLength, AnyValue, EndOfRecord; 
EndOfRecord     = 'E', '\n';

AuVersion       = UintValue;
RelDictLocation = EncodedInt;
ValueLength     = EncodedInt;
NonHeaderRecord = ClearDictRecord | AddDictRecord | ValueRecord;

UintValue       = 'I', EncodedInt;
SintValue       = 'J', EncodedInt;
TrueValue       = 'T';
FalseValue      = 'F';
NullValue       = 'N';
DoubleValue     = 'D', LEDouble;
StringValue     = InternString | InlineString;
ObjectValue     = '{', {KeyValue, AnyValue}, '}'
ArrayValue      = '[', {AnyValue}, ']' 
KeyValue        = StringValue
AnyValue        = UintValue | SintValue | TrueValue | FalseValue | NullValue | DoubleValue | StringValue;

InternString    = 'X', InternId;
InlineString    = 'S', StringLen, {Bytes};
InternId        = EncodedInt
StringLen       = EncodedInt
LEDouble        = ? a double value - 8 bytes little-endian ?
```

Explanations
============
- `RelDictLocation` is the offset from the start of the `ValueRecord` to the previous `ClearDictRecord` or `AddDictRecord` record. 
- `ClearDictRecord` invalidates all the interned string IDs
- `AddDictRecord` adds one or more strings to the interned string dictionary
- `Bytes` can be any byte sequence.
- `SintValue` encodes a negative value. The `EncodedInt` must be decoded and multiplied by -1 to recover the original integer value.