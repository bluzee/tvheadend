tvheadend.timeshift = function(panel, index) {

    /* ****************************************************************
     * Data
     * ***************************************************************/

    var confreader = new Ext.data.JsonReader(
            {
                root: 'config'
            },
    [
        'timeshift_enabled', 'timeshift_ondemand',
        'timeshift_path',
        'timeshift_unlimited_period', 'timeshift_max_period',
        'timeshift_unlimited_size', 'timeshift_max_size'
    ]
            );

    /* ****************************************************************
     * Fields
     * ***************************************************************/

    var timeshiftEnabled = new Ext.form.Checkbox({
        fieldLabel: 'Enabled',
        name: 'timeshift_enabled',
        width: 300
    });

    var timeshiftOndemand = new Ext.form.Checkbox({
        fieldLabel: 'On-Demand',
        name: 'timeshift_ondemand',
        width: 300
    });

    var timeshiftPath = new Ext.form.TextField({
        fieldLabel: 'Storage Path',
        name: 'timeshift_path',
        allowBlank: true,
        width: 300
    });

    var timeshiftMaxPeriod = new Ext.form.NumberField({
        fieldLabel: 'Max. Period (mins)',
        name: 'timeshift_max_period',
        allowBlank: false,
        width: 300
    });

    var timeshiftUnlPeriod = new Ext.form.Checkbox({
        fieldLabel: 'Unlimited time',
        name: 'timeshift_unlimited_period',
        width: 300
    });

    var timeshiftMaxSize = new Ext.form.NumberField({
        fieldLabel: 'Max. Size (MB)',
        name: 'timeshift_max_size',
        allowBlank: false,
        width: 300
    });

    var timeshiftUnlSize = new Ext.form.Checkbox({
        fieldLabel: 'Unlimited size',
        name: 'timeshift_unlimited_size',
        width: 300
    });

    /* ****************************************************************
     * Events
     * ***************************************************************/

    timeshiftUnlPeriod.on('check', function(e, c){
        timeshiftMaxPeriod.setDisabled(c);
    });

    timeshiftUnlSize.on('check', function(e, c){
        timeshiftMaxSize.setDisabled(c);
    });

    /* ****************************************************************
     * Form
     * ***************************************************************/

    var saveButton = new Ext.Button({
        text: "Save configuration",
        tooltip: 'Save changes made to configuration below',
        iconCls: 'save',
        handler: saveChanges
    });

    var helpButton = new Ext.Button({
        text: 'Help',
        handler: function() {
            new tvheadend.help('Timeshift Configuration', 'config_timeshift.html');
        }
    });

    var timeshiftPanelA = new Ext.form.FieldSet({
        width: 500,
        autoHeight: true,
        border: false,
  	    items : [timeshiftMaxPeriod, timeshiftMaxSize]
    });

    var timeshiftPanelB = new Ext.form.FieldSet({
        width: 200,
        autoHeight: true,
        border: false,
        items : [timeshiftUnlPeriod,timeshiftUnlSize]
    });

    var timeshiftPanel = new Ext.form.FieldSet({
        title: 'Timeshift Options',
        width: 700,
        autoHeight: true,
        collapsible: true,
        animCollapse: true,
        items : [
            timeshiftEnabled, 
            timeshiftOndemand, 
            timeshiftPath,
            {
                layout: 'column', 
                border: false,
                items: [timeshiftPanelA, timeshiftPanelB]
            }	
        ]
    });

    var confpanel = new Ext.form.FormPanel({
        title : 'Timeshift',
        iconCls : 'clock',
        border : false,
        bodyStyle : 'padding:15px',
        labelAlign : 'left',
        labelWidth : 150,
        waitMsgTarget : true,
        reader : confreader,
        layout : 'form',
        defaultType : 'textfield',
        autoHeight : true,
        animCollapse : true,
    	items : [timeshiftPanel],
        tbar : [saveButton, '->', helpButton]
    });

    /* ****************************************************************
     * Load/Save
     * ***************************************************************/

    confpanel.on('render', function() {
        confpanel.getForm().load({
            url: 'timeshift',
            params: {
                'op': 'loadSettings'
            },
            success: function() {
                confpanel.enable();
                timeshiftMaxPeriod.setDisabled(timeshiftUnlPeriod.getValue());
                timeshiftMaxSize.setDisabled(timeshiftUnlSize.getValue());
            }
        });
    });

    function saveChanges() {
        confpanel.getForm().submit({
            url: 'timeshift',
            params: {
                op: 'saveSettings'
            },
            waitMsg: 'Saving Data...',
            success: function(form, action) {
            },
            failure: function(form, action) {
                Ext.Msg.alert('Save failed', action.result.errormsg);
            }
        });
    }

    tvheadend.paneladd(panel, confpanel, index);
};
